// VipleStream §J.3.e.X — hand-rolled RIFE Vulkan: param parser + scaffold.
//
// See header for full plan.  This translation unit only implements the
// .param parser + smoke entry point.  No GPU work yet.

#include "rife_native_vk.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
// Windows.h (pulled in transitively by vulkan.h on Win32) defines min/max
// macros that break std::max / std::numeric_limits<>::max() in this TU.
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif
#include <vulkan/vulkan.h>
#include <ncnn/gpu.h>
#ifndef _WIN32
#  include <dlfcn.h>
#endif

namespace viple::rife_native_vk {

const char* opKindName(OpKind k) {
    switch (k) {
        case OpKind::Input:         return "Input";
        case OpKind::MemoryData:    return "MemoryData";
        case OpKind::Split:         return "Split";
        case OpKind::Convolution:   return "Convolution";
        case OpKind::Deconvolution: return "Deconvolution";
        case OpKind::BinaryOp:      return "BinaryOp";
        case OpKind::ReLU:          return "ReLU";
        case OpKind::Crop:          return "Crop";
        case OpKind::Concat:        return "Concat";
        case OpKind::Interp:        return "Interp";
        case OpKind::PixelShuffle:  return "PixelShuffle";
        case OpKind::Sigmoid:       return "Sigmoid";
        case OpKind::Eltwise:       return "Eltwise";
        case OpKind::RifeWarp:      return "rife.Warp";
        default:                    return "Unknown";
    }
}

static OpKind parseOpKind(const QString& s) {
    if (s == "Input")         return OpKind::Input;
    if (s == "MemoryData")    return OpKind::MemoryData;
    if (s == "Split")         return OpKind::Split;
    if (s == "Convolution")   return OpKind::Convolution;
    if (s == "Deconvolution") return OpKind::Deconvolution;
    if (s == "BinaryOp")      return OpKind::BinaryOp;
    if (s == "ReLU")          return OpKind::ReLU;
    if (s == "Crop")          return OpKind::Crop;
    if (s == "Concat")        return OpKind::Concat;
    if (s == "Interp")        return OpKind::Interp;
    if (s == "PixelShuffle")  return OpKind::PixelShuffle;
    if (s == "Sigmoid")       return OpKind::Sigmoid;
    if (s == "Eltwise")       return OpKind::Eltwise;
    if (s == "rife.Warp")     return OpKind::RifeWarp;
    return OpKind::Unknown;
}

// Parse a single param token of the form "id=value" or "-23xxx=count,v1,v2,..."
// into the out map.  Returns true on success.
static bool parseParamToken(const QString& tok, std::unordered_map<int, ParamValue>& out) {
    int eq = tok.indexOf('=');
    if (eq <= 0) return false;
    bool ok = false;
    int id = tok.left(eq).toInt(&ok);
    if (!ok) return false;
    QString rhs = tok.mid(eq + 1);
    ParamValue pv;
    // Negative ids in the -23300..-23399 range mean "array of N values":
    // first element is the count, remaining are the values.  ncnn's
    // convention; see e.g. Crop's -23309 / -23310 / -23311.
    if (id <= -23300 && id >= -23399) {
        QStringList parts = rhs.split(',', Qt::SkipEmptyParts);
        if (parts.isEmpty()) return false;
        // Heuristic: try int first; if any element looks like float
        // (has '.' or 'e') treat the whole array as float.
        bool hasFloat = false;
        for (const QString& p : parts) {
            if (p.contains('.') || p.contains('e') || p.contains('E')) {
                hasFloat = true; break;
            }
        }
        if (hasFloat) {
            pv.kind = ParamValue::FloatArray;
            for (int j = 1; j < parts.size(); ++j) {
                pv.fa.push_back(parts[j].toDouble());
            }
        } else {
            pv.kind = ParamValue::IntArray;
            for (int j = 1; j < parts.size(); ++j) {
                pv.ia.push_back(parts[j].toLongLong());
            }
        }
    } else if (rhs.contains('.') || rhs.contains('e') || rhs.contains('E')) {
        pv.kind = ParamValue::Float;
        pv.f    = rhs.toDouble();
    } else {
        pv.kind = ParamValue::Int;
        pv.i    = rhs.toLongLong();
    }
    out[id] = pv;
    return true;
}

// ---- Common layer / param helpers (used by Phase 3+/4*/4g) ----

// Lookup a layer by exact name in the parsed model.  Returns nullptr
// if absent.
static const Layer* findLayerByName(const Model& m, const QString& name) {
    for (const auto& L : m.layers) {
        if (L.name == name) return &L;
    }
    return nullptr;
}

// Read an int param with default if absent.
static int paramInt(const Layer& L, int id, int def) {
    auto it = L.params.find(id);
    if (it == L.params.end()) return def;
    return (int)it->second.i;
}

// Read the LeakyReLU slope from activation_params (-23310 array) when
// activation_type == 2.  Returns 0.0f when no LeakyReLU.
static float leakyReluSlopeOf(const Layer& L) {
    int actType = paramInt(L, 9, 0);
    if (actType != 2) return 0.0f;
    auto it = L.params.find(-23310);
    if (it == L.params.end()) return 0.0f;
    if (it->second.fa.empty()) return 0.0f;
    return (float)it->second.fa[0];
}

bool parseParam(const QString& path, Model& out) {
    out = {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] parseParam: cannot open %s",
                     qUtf8Printable(path));
        return false;
    }
    QTextStream ts(&f);
    QString line;

    // Line 1: magic
    line = ts.readLine().trimmed();
    out.magic = line.toInt();
    if (out.magic != 7767517) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] parseParam: bad magic %d (expected 7767517)",
                     out.magic);
        return false;
    }

    // Line 2: layer_count blob_count
    line = ts.readLine().trimmed();
    QStringList hdr = line.split(' ', Qt::SkipEmptyParts);
    if (hdr.size() != 2) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] parseParam: bad header line: %s",
                     qUtf8Printable(line));
        return false;
    }
    out.layerCount = hdr[0].toInt();
    out.blobCount  = hdr[1].toInt();
    out.layers.reserve(out.layerCount);

    // Layer rows
    int unknownOpCount = 0;
    while (!ts.atEnd()) {
        line = ts.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList toks = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (toks.size() < 4) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RIFE-VK] parseParam: short line, skip: %s",
                        qUtf8Printable(line));
            continue;
        }
        Layer L;
        L.opType        = toks[0];
        L.name          = toks[1];
        int inCount     = toks[2].toInt();
        int outCount    = toks[3].toInt();
        if (toks.size() < 4 + inCount + outCount) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RIFE-VK] parseParam: truncated layer %s",
                        qUtf8Printable(L.name));
            continue;
        }
        for (int j = 0; j < inCount; ++j)  L.inputs  << toks[4 + j];
        for (int j = 0; j < outCount; ++j) L.outputs << toks[4 + inCount + j];
        // Remaining tokens are param_id=value pairs.
        for (int j = 4 + inCount + outCount; j < toks.size(); ++j) {
            parseParamToken(toks[j], L.params);
        }
        L.kind = parseOpKind(L.opType);
        if (L.kind == OpKind::Unknown) unknownOpCount++;
        out.opCounts[L.kind]++;
        out.layers.push_back(std::move(L));
    }

    if (unknownOpCount > 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] parseParam: %d layers had unrecognised op type "
                    "(graph executor will fail on these later)", unknownOpCount);
    }
    if ((int)out.layers.size() != out.layerCount) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] parseParam: parsed %zu layers but header says %d",
                    out.layers.size(), out.layerCount);
    }
    return true;
}

// §J.3.e.X Phase 3b.1 — Conv2D 3×3 GLSL compute shader.
//
// Generic enough to handle any kernelW/kernelH (no compile-time
// specialisation yet).  Dispatch covers N output channels in z-dim,
// outH/8 in y-dim, outW/8 in x-dim.  Each thread reads (kernelH ×
// kernelW × inC) input pixels and accumulates to one output pixel.
//
// Trade-offs accepted for Phase 3b.1 (will revisit as we profile):
//   • No tiling / shared-memory cache — input read directly from
//     storage buffer with O(kernel² × inC) reads per output thread.
//     For 3×3 conv with inC=3 that's 27 reads (cheap); for inC=192
//     residual block it's 1728 reads (potentially memory-bound).
//   • No fp16 weight in shader yet — host pre-unpacks fp16→fp32 into
//     the weight buffer.  Saves shader complexity at cost of 2× weight
//     buffer memory.  fp16 in-shader is a Phase 3c optimisation.
//   • Bounds-check at every load (instead of separate edge / interior
//     dispatches).  Branchy but trivial cost on modern GPUs.
//
// Shader string is checked in here for source review; ncnn's
// compile_spirv_module turns it into SPIR-V at runtime.  Future phase
// will switch to glslangValidator at build time + embedded SPIR-V to
// cut the runtime ncnn dependency.
static const char* kConv2DShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) readonly buffer InputBuf  { float in_buf[];  };
layout(set = 0, binding = 1) readonly buffer WeightBuf { float w_buf[];   };
layout(set = 0, binding = 2) readonly buffer BiasBuf   { float bias_buf[]; };
layout(set = 0, binding = 3) writeonly buffer OutputBuf { float out_buf[]; };

layout(push_constant) uniform PC {
    ivec4 inDims;     // (inW, inH, inC, hasBias)
    ivec4 outDims;    // (outW, outH, outChan, _pad)
    ivec4 conv;       // (kernelW, kernelH, stride, pad)
    float leakyReluSlope;
} pc;

void main() {
    int ox = int(gl_GlobalInvocationID.x);
    int oy = int(gl_GlobalInvocationID.y);
    int n  = int(gl_GlobalInvocationID.z);
    if (ox >= pc.outDims.x || oy >= pc.outDims.y || n >= pc.outDims.z) {
        return;
    }
    int inW   = pc.inDims.x;
    int inH   = pc.inDims.y;
    int inC   = pc.inDims.z;
    int hasB  = pc.inDims.w;
    int outW  = pc.outDims.x;
    int outH  = pc.outDims.y;
    int outN  = pc.outDims.z;
    int kW    = pc.conv.x;
    int kH    = pc.conv.y;
    int stride = pc.conv.z;
    int pad   = pc.conv.w;

    float acc = (hasB != 0) ? bias_buf[n] : 0.0;

    // weight indexed [n, c, ky, kx] in NCHW layout; flatten to
    // ((n * inC + c) * kH + ky) * kW + kx.
    int wRowStride = kW;
    int wPlaneStride = kH * kW;
    int wChanStride = inC * wPlaneStride;

    for (int c = 0; c < inC; ++c) {
        int wChanBase = n * wChanStride + c * wPlaneStride;
        int inChanBase = c * inH * inW;
        for (int ky = 0; ky < kH; ++ky) {
            int iy = oy * stride + ky - pad;
            if (iy < 0 || iy >= inH) continue;
            for (int kx = 0; kx < kW; ++kx) {
                int ix = ox * stride + kx - pad;
                if (ix < 0 || ix >= inW) continue;
                float wv = w_buf[wChanBase + ky * wRowStride + kx];
                float iv = in_buf[inChanBase + iy * inW + ix];
                acc += wv * iv;
            }
        }
    }

    if (pc.leakyReluSlope > 0.0 && acc < 0.0) {
        acc *= pc.leakyReluSlope;
    }
    out_buf[(n * outH + oy) * outW + ox] = acc;
}
)GLSL";

const char* getConv2DShaderGlsl() {
    return kConv2DShaderGlsl;
}

// ============================================================================
// §J.3.e.X Phase 4b — element-wise BinaryOp.
//
// Covers 92 BinaryOp layers in rife-v4.25-lite/flownet.param.  Audit:
//   ops:         ADD (43)  MUL (45)  DIV (3)  RSUB (1)
//   forms:       same-shape NCHW⊕NCHW           (85 layers, in_count=2)
//                channel broadcast NCHW × C     (subset of above for MUL × beta)
//                scalar  NCHW ⊕ const           (7 layers, with_scalar=1)
//
// One unified shader handles all cases via a `mode` push constant:
//   mode 0 = same-shape   :  y[i] = a[i] OP b[i]
//   mode 1 = channel-bcast:  y[i] = a[i] OP b[chan(i)]   where chan(i) = (i / hw) % C
//   mode 2 = scalar       :  y[i] = a[i] OP scalar_b
//
// op_type uses ncnn's IDs directly:
//   0 ADD   1 SUB   2 MUL   3 DIV   4 MAX   5 MIN
//   6 POW   7 RSUB  8 RDIV  9 RPOW
// (Only ADD / SUB / MUL / DIV / RSUB are reachable in this model.)
//
// Dispatch: 1D over the total element count of a (gl_GlobalInvocationID.x
// = linear index).  local_size_x = 64.  For tensors larger than
// maxComputeWorkGroupCount[0]×64 the executor will need to split into
// 2D dispatch — out of scope for the correctness gate; small inputs
// only.
// ============================================================================

static const char* kBinaryOpShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) readonly  buffer InA { float a_buf[]; };
layout(set = 0, binding = 1) readonly  buffer InB { float b_buf[]; };
layout(set = 0, binding = 2) writeonly buffer Out { float y_buf[]; };

layout(push_constant) uniform PC {
    uint count;       // total elements in a (== output count)
    uint hw;          // h*w           (used in mode 1 channel broadcast)
    uint channels;    // C             (used in mode 1)
    int  op_type;     // ncnn op_type
    int  mode;        // 0 same / 1 chan-bcast / 2 scalar
    float scalar_b;   // mode 2 only
} pc;

float apply_op(float aa, float bb) {
    if (pc.op_type == 0) return aa + bb;          // ADD
    if (pc.op_type == 1) return aa - bb;          // SUB
    if (pc.op_type == 2) return aa * bb;          // MUL
    if (pc.op_type == 3) return aa / bb;          // DIV
    if (pc.op_type == 4) return max(aa, bb);      // MAX
    if (pc.op_type == 5) return min(aa, bb);      // MIN
    if (pc.op_type == 6) return pow(aa, bb);      // POW
    if (pc.op_type == 7) return bb - aa;          // RSUB
    if (pc.op_type == 8) return bb / aa;          // RDIV
    if (pc.op_type == 9) return pow(bb, aa);      // RPOW
    return aa;
}

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    float aa = a_buf[i];
    float bb;
    if (pc.mode == 2) {
        bb = pc.scalar_b;
    } else if (pc.mode == 1) {
        // channel broadcast: i is linear NCHW index → channel = (i / hw) % C
        bb = b_buf[(i / pc.hw) % pc.channels];
    } else {
        bb = b_buf[i];
    }
    y_buf[i] = apply_op(aa, bb);
}
)GLSL";

const char* getBinaryOpShaderGlsl() {
    return kBinaryOpShaderGlsl;
}

// ============================================================================
// §J.3.e.X Phase 4c — element-wise Activation (ReLU / LeakyReLU / Sigmoid).
//
// Covers 41 activation layers in rife-v4.25-lite/flownet.param:
//   ReLU  × 40  — all with slope=0.2 (i.e. LeakyReLU 0.2)
//   Sigmoid × 1  — output mask scaling at the very end of the network
//
// Note: 16 Convolution layers also fuse LeakyReLU 0.2 directly (already
// handled by the Conv2D shader's `leakyReluSlope` push constant); this
// shader is for the **standalone** ReLU layers + the single Sigmoid.
//
// One unified shader covers both via `actType` push constant:
//   0 = ReLU / LeakyReLU  (slope param controls negative-side scale)
//   1 = Sigmoid            (slope unused)
//
// Dispatch: 1D over total element count, local_size_x = 64.
// ============================================================================

static const char* kActivationShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) readonly  buffer InBuf  { float in_buf[];  };
layout(set = 0, binding = 1) writeonly buffer OutBuf { float out_buf[]; };

layout(push_constant) uniform PC {
    uint  count;
    int   actType;    // 0 ReLU/LeakyReLU, 1 Sigmoid
    float slope;      // negative-side multiplier for ReLU; ignored for Sigmoid
} pc;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    float x = in_buf[i];
    float y;
    if (pc.actType == 0) {
        y = (x < 0.0) ? x * pc.slope : x;
    } else if (pc.actType == 1) {
        y = 1.0 / (1.0 + exp(-x));
    } else {
        y = x;
    }
    out_buf[i] = y;
}
)GLSL";

const char* getActivationShaderGlsl() {
    return kActivationShaderGlsl;
}

// ============================================================================
// §J.3.e.X Phase 4d — shape ops: Copy / PixelShuffle / InterpBilinear /
// EltwiseSum.
//
// Coverage:
//   Concat × 9    — N applications of kCopyShader, each writing one input
//                   into output at growing channel offset.
//   Crop  × 45    — single application of kCopyShader reading from a
//                   channel-offset slice.  Audit confirmed all 45 crops
//                   slice along channel axis only (no H/W cropping).
//   Interp × 14   — bilinear resize, all resize_type=2.
//   PixelShuffle × 5 — depth-to-space, all upscale_factor=2.
//   Eltwise × 3   — weighted SUM of 2 inputs with coeffs (all in this
//                   model are SUM with coeffs [1.0, k] for k ∈ {4, 8, 16}).
// ============================================================================

// Plain memcpy via compute shader, with src/dst element offsets.
// Used by Concat (multi-pass) and Crop (single pass).
static const char* kCopyShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) readonly  buffer InBuf  { float in_buf[];  };
layout(set = 0, binding = 1) writeonly buffer OutBuf { float out_buf[]; };
layout(push_constant) uniform PC {
    uint count;
    uint srcOffset;
    uint dstOffset;
} pc;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    out_buf[pc.dstOffset + i] = in_buf[pc.srcOffset + i];
}
)GLSL";
const char* getCopyShaderGlsl() { return kCopyShaderGlsl; }

// Depth-to-space (PixelShuffle).  Input (Cin, Hin, Win) → output
// (Cin/r², Hin*r, Win*r).  For each output pixel:
//   c_in = c_out * r² + (h_out % r) * r + (w_out % r)
//   h_in = h_out / r ; w_in = w_out / r
static const char* kPixelShuffleShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) readonly  buffer InBuf  { float in_buf[];  };
layout(set = 0, binding = 1) writeonly buffer OutBuf { float out_buf[]; };
layout(push_constant) uniform PC {
    int inH;
    int inW;
    int outC;   // = inC / (r*r)
    int r;
} pc;
void main() {
    int ow = int(gl_GlobalInvocationID.x);
    int oh = int(gl_GlobalInvocationID.y);
    int oc = int(gl_GlobalInvocationID.z);
    int outH = pc.inH * pc.r;
    int outW = pc.inW * pc.r;
    if (ow >= outW || oh >= outH || oc >= pc.outC) return;
    int ic = oc * (pc.r * pc.r) + (oh % pc.r) * pc.r + (ow % pc.r);
    int ih = oh / pc.r;
    int iw = ow / pc.r;
    float v = in_buf[(ic * pc.inH + ih) * pc.inW + iw];
    out_buf[(oc * outH + oh) * outW + ow] = v;
}
)GLSL";
const char* getPixelShuffleShaderGlsl() { return kPixelShuffleShaderGlsl; }

// Bilinear resize (Interp), align_corners=false convention:
//   in_f = (out + 0.5) * (inDim / outDim) - 0.5
// Edge-clamp to [0, dim-1].
static const char* kInterpBilinearShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(set = 0, binding = 0) readonly  buffer InBuf  { float in_buf[];  };
layout(set = 0, binding = 1) writeonly buffer OutBuf { float out_buf[]; };
layout(push_constant) uniform PC {
    int   inH;
    int   inW;
    int   outH;
    int   outW;
    int   channels;
    float scaleH;   // inH / outH
    float scaleW;   // inW / outW
} pc;
void main() {
    int ow = int(gl_GlobalInvocationID.x);
    int oh = int(gl_GlobalInvocationID.y);
    int c  = int(gl_GlobalInvocationID.z);
    if (ow >= pc.outW || oh >= pc.outH || c >= pc.channels) return;

    float inh_f = (float(oh) + 0.5) * pc.scaleH - 0.5;
    float inw_f = (float(ow) + 0.5) * pc.scaleW - 0.5;
    int   ih0   = int(floor(inh_f));
    int   iw0   = int(floor(inw_f));
    float fh    = inh_f - float(ih0);
    float fw    = inw_f - float(iw0);
    int   ih1   = ih0 + 1;
    int   iw1   = iw0 + 1;
    ih0 = clamp(ih0, 0, pc.inH - 1);
    ih1 = clamp(ih1, 0, pc.inH - 1);
    iw0 = clamp(iw0, 0, pc.inW - 1);
    iw1 = clamp(iw1, 0, pc.inW - 1);

    int planeBase = c * pc.inH * pc.inW;
    float v00 = in_buf[planeBase + ih0 * pc.inW + iw0];
    float v01 = in_buf[planeBase + ih0 * pc.inW + iw1];
    float v10 = in_buf[planeBase + ih1 * pc.inW + iw0];
    float v11 = in_buf[planeBase + ih1 * pc.inW + iw1];
    float v0  = v00 * (1.0 - fw) + v01 * fw;
    float v1  = v10 * (1.0 - fw) + v11 * fw;
    float v   = v0  * (1.0 - fh) + v1  * fh;

    out_buf[(c * pc.outH + oh) * pc.outW + ow] = v;
}
)GLSL";
const char* getInterpBilinearShaderGlsl() { return kInterpBilinearShaderGlsl; }

// Eltwise SUM with coefficients: out[i] = c0 * a[i] + c1 * b[i].
// Only 2-input sum form is reachable in flownet (3 layers, all SUM).
static const char* kEltwiseShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) readonly  buffer InA  { float a_buf[]; };
layout(set = 0, binding = 1) readonly  buffer InB  { float b_buf[]; };
layout(set = 0, binding = 2) writeonly buffer Out  { float y_buf[]; };
layout(push_constant) uniform PC {
    uint  count;
    float c0;
    float c1;
} pc;
void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= pc.count) return;
    y_buf[i] = pc.c0 * a_buf[i] + pc.c1 * b_buf[i];
}
)GLSL";
const char* getEltwiseShaderGlsl() { return kEltwiseShaderGlsl; }

// ============================================================================
// §J.3.e.X Phase 4e — Deconvolution (transposed conv).
//
// Audit confirmed all 7 Deconv layers in flownet.param are uniform:
//   k=4×4, stride=2, pad=1, bias=on, no activation, dilation=1.
//   Output dim = (inDim - 1) * 2 + 4 - 2 = 2 * inDim  (exact 2× upsample)
//
// Weight layout for ncnn Deconvolution is (in_ch, out_ch, kH, kW),
// i.e. axes 0/1 are SWAPPED relative to Convolution's (out_ch, in_ch,
// kH, kW).  The shader's weight index reflects this.
//
// Math (standard transposed conv, reads input "backwards" through
// stride):
//   out[n, oh, ow] = bias[n]
//     + Σ_{c, ky, kx} in[c, ih, iw] * weight[c, n, ky, kx]
//   where  ih_num = oh + pad - ky;  iw_num = ow + pad - kx
//          contribute only if (ih_num >= 0 && ih_num % stride == 0
//                          && (ih = ih_num / stride) < inH)
//          and same for iw.
// ============================================================================

static const char* kDeconv2DShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) readonly buffer InputBuf  { float in_buf[];   };
layout(set = 0, binding = 1) readonly buffer WeightBuf { float w_buf[];    };
layout(set = 0, binding = 2) readonly buffer BiasBuf   { float bias_buf[]; };
layout(set = 0, binding = 3) writeonly buffer OutputBuf { float out_buf[]; };

layout(push_constant) uniform PC {
    ivec4 inDims;     // (inW, inH, inC, hasBias)
    ivec4 outDims;    // (outW, outH, outChan, _pad)
    ivec4 conv;       // (kernelW, kernelH, stride, pad)
} pc;

void main() {
    int ow = int(gl_GlobalInvocationID.x);
    int oh = int(gl_GlobalInvocationID.y);
    int n  = int(gl_GlobalInvocationID.z);
    if (ow >= pc.outDims.x || oh >= pc.outDims.y || n >= pc.outDims.z) return;

    int inW   = pc.inDims.x;
    int inH   = pc.inDims.y;
    int inC   = pc.inDims.z;
    int hasB  = pc.inDims.w;
    int outW  = pc.outDims.x;
    int outH  = pc.outDims.y;
    int outN  = pc.outDims.z;
    int kW    = pc.conv.x;
    int kH    = pc.conv.y;
    int strd  = pc.conv.z;
    int pad   = pc.conv.w;

    float acc = (hasB != 0) ? bias_buf[n] : 0.0;

    // Weight indexed (c, n, ky, kx) (note ncnn Deconv has in_ch outer):
    //   w[((c * outN + n) * kH + ky) * kW + kx]
    int wPlaneStride = kH * kW;

    for (int c = 0; c < inC; ++c) {
        int wChanBase  = (c * outN + n) * wPlaneStride;
        int inChanBase = c * inH * inW;
        for (int ky = 0; ky < kH; ++ky) {
            int ihNum = oh + pad - ky;
            if (ihNum < 0) continue;
            if ((ihNum % strd) != 0) continue;
            int ih = ihNum / strd;
            if (ih >= inH) continue;
            for (int kx = 0; kx < kW; ++kx) {
                int iwNum = ow + pad - kx;
                if (iwNum < 0) continue;
                if ((iwNum % strd) != 0) continue;
                int iw = iwNum / strd;
                if (iw >= inW) continue;
                float wv = w_buf[wChanBase + ky * kW + kx];
                float iv = in_buf[inChanBase + ih * inW + iw];
                acc += wv * iv;
            }
        }
    }

    out_buf[(n * outH + oh) * outW + ow] = acc;
}
)GLSL";

const char* getDeconv2DShaderGlsl() { return kDeconv2DShaderGlsl; }

// ============================================================================
// §J.3.e.X Phase 4f — rife.Warp (custom op).
//
// Optical-flow-driven bilinear resample.  Math reverse-engineered from
// ncnn_rife_warp.cpp's GLSL (kWarpComp) — itself a verbatim copy of
// rife-ncnn-vulkan/src/warp.comp (BSD-3-Clause, nihui/ncnn).
//
// Inputs (NCHW, but packed flat fp32 in our layout):
//   image (C, H, W)  — feature map to warp
//   flow  (2, H, W)  — channel 0 = flow_x, channel 1 = flow_y
//
// For each output (c, y, x):
//   sx = x + flow_x[y,x] ; sy = y + flow_y[y,x]
//   x0,y0 = floor(sx,sy);  x1=x0+1; y1=y0+1
//   clamp x0/x1 to [0, w-1] and y0/y1 to [0, h-1]
//   alpha = sx - x0;  beta = sy - y0
//   v = bilinear interp of image[c] at (x0..x1, y0..y1)
//
// 18 rife.Warp layers in flownet — all share identical math (the layer
// has no learnable params, only the (w, h, c, cstep) push constants
// derived from input shape at runtime).
// ============================================================================

static const char* kRifeWarpShaderGlsl = R"GLSL(
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) readonly  buffer ImageBuf { float img_buf[];  };
layout(set = 0, binding = 1) readonly  buffer FlowBuf  { float flow_buf[]; };
layout(set = 0, binding = 2) writeonly buffer OutBuf   { float out_buf[];  };

layout(push_constant) uniform PC {
    int w;
    int h;
    int c;
} pc;

void main() {
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);
    if (gx >= pc.w || gy >= pc.h || gz >= pc.c) return;

    int hw = pc.h * pc.w;
    float fx = flow_buf[0 * hw + gy * pc.w + gx];
    float fy = flow_buf[1 * hw + gy * pc.w + gx];

    float sx = float(gx) + fx;
    float sy = float(gy) + fy;
    int x0 = int(floor(sx));
    int y0 = int(floor(sy));
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    x0 = clamp(x0, 0, pc.w - 1);
    y0 = clamp(y0, 0, pc.h - 1);
    x1 = clamp(x1, 0, pc.w - 1);
    y1 = clamp(y1, 0, pc.h - 1);
    float alpha = sx - float(x0);
    float beta  = sy - float(y0);

    int planeBase = gz * hw;
    float v0 = img_buf[planeBase + y0 * pc.w + x0];
    float v1 = img_buf[planeBase + y0 * pc.w + x1];
    float v2 = img_buf[planeBase + y1 * pc.w + x0];
    float v3 = img_buf[planeBase + y1 * pc.w + x1];
    float v4 = v0 * (1.0 - alpha) + v1 * alpha;
    float v5 = v2 * (1.0 - alpha) + v3 * alpha;
    out_buf[planeBase + gy * pc.w + gx] = v4 * (1.0 - beta) + v5 * beta;
}
)GLSL";

const char* getRifeWarpShaderGlsl() { return kRifeWarpShaderGlsl; }

// IEEE-754 fp16 → fp32 conversion.  Handles ±0, normals, and ±inf/NaN.
// Subnormals (e==0, m!=0) are flushed to ±0 — acceptable for trained
// conv weights where we never see subnormals in practice.
//
// Layout reference:
//   fp16: [s:1] [e:5] [m:10]  bias = 15
//   fp32: [s:1] [e:8] [m:23]  bias = 127
static float fp16ToFp32(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16;
    uint32_t e = (uint32_t)(h >> 10) & 0x1Fu;
    uint32_t m = (uint32_t)(h & 0x3FFu);
    uint32_t v;
    if (e == 0) {
        // ±0 (m==0) or subnormal (m!=0) → flush to ±0
        v = s;
    } else if (e == 31) {
        // inf (m==0) or NaN (m!=0)
        v = s | 0x7F800000u | (m << 13);
    } else {
        // normal: rebias exponent 15 → 127, shift mantissa 10 → 23
        v = s | ((e + (127 - 15)) << 23) | (m << 13);
    }
    float r;
    std::memcpy(&r, &v, sizeof(r));
    return r;
}

std::vector<float> getTensorAsFp32(const Model& m, const QString& tensorName) {
    auto it = m.tensorByName.find(tensorName);
    if (it == m.tensorByName.end()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] getTensorAsFp32: tensor '%s' not found",
                    qUtf8Printable(tensorName));
        return {};
    }
    const WeightTensor& t = m.tensors[it->second];
    std::vector<float> out;
    out.reserve(t.elemCount);
    if (t.byteOffset > m.weightBlob.size()) return {};
    if (t.dtype == TensorDType::Float32) {
        if (t.byteOffset + t.elemCount * sizeof(float) > m.weightBlob.size()) return {};
        const float* p = reinterpret_cast<const float*>(m.weightBlob.data() + t.byteOffset);
        out.assign(p, p + t.elemCount);
    } else { // Float16
        if (t.byteOffset + t.elemCount * sizeof(uint16_t) > m.weightBlob.size()) return {};
        const uint16_t* p = reinterpret_cast<const uint16_t*>(m.weightBlob.data() + t.byteOffset);
        for (size_t i = 0; i < t.elemCount; ++i) {
            out.push_back(fp16ToFp32(p[i]));
        }
    }
    return out;
}

void referenceConv2D(const float* in, int inW, int inH, int inC,
                     const float* weight, int outChan, int kernelH, int kernelW,
                     const float* bias,
                     int strideH, int strideW, int padH, int padW,
                     float leakyReluSlope,
                     float* out, int outW, int outH) {
    // weight layout: [N, C, kH, kW] (NCHW) — matches ncnn / PyTorch.
    // input layout : [C, H, W]
    // output layout: [N, outH, outW]
    //
    // Each output cell (n, oy, ox) accumulates inC * kH * kW MAC ops.
    // Boundary pixels (oy*stride - pad + ky outside [0, inH)) treated
    // as zero (zero-padded).  Bias added per-output-channel.
    for (int n = 0; n < outChan; ++n) {
        const float biasVal = bias ? bias[n] : 0.0f;
        for (int oy = 0; oy < outH; ++oy) {
            for (int ox = 0; ox < outW; ++ox) {
                float acc = biasVal;
                for (int c = 0; c < inC; ++c) {
                    for (int ky = 0; ky < kernelH; ++ky) {
                        int iy = oy * strideH + ky - padH;
                        if (iy < 0 || iy >= inH) continue;
                        for (int kx = 0; kx < kernelW; ++kx) {
                            int ix = ox * strideW + kx - padW;
                            if (ix < 0 || ix >= inW) continue;
                            float w = weight[((n * inC + c) * kernelH + ky) * kernelW + kx];
                            float v = in[(c * inH + iy) * inW + ix];
                            acc += w * v;
                        }
                    }
                }
                if (leakyReluSlope > 0.0f && acc < 0.0f) acc *= leakyReluSlope;
                out[(n * outH + oy) * outW + ox] = acc;
            }
        }
    }
}

// Phase 4e — Deconvolution (transposed conv) CPU reference.
// Weight layout: (in_ch, out_ch, kH, kW) — note ic OUTER, swapped vs Conv.
// Output dim: outH = (inH - 1) * strideH + kH - padH_top - padH_bottom
//             (assumes padTop == padBottom == padH; same for W).
void referenceDeconv2D(const float* in, int inW, int inH, int inC,
                       const float* weight, int outChan, int kernelH, int kernelW,
                       const float* bias,
                       int strideH, int strideW, int padH, int padW,
                       float* out, int outW, int outH) {
    for (int n = 0; n < outChan; ++n) {
        const float biasVal = bias ? bias[n] : 0.0f;
        for (int oy = 0; oy < outH; ++oy) {
            for (int ox = 0; ox < outW; ++ox) {
                float acc = biasVal;
                for (int c = 0; c < inC; ++c) {
                    for (int ky = 0; ky < kernelH; ++ky) {
                        int ihNum = oy + padH - ky;
                        if (ihNum < 0) continue;
                        if ((ihNum % strideH) != 0) continue;
                        int ih = ihNum / strideH;
                        if (ih >= inH) continue;
                        for (int kx = 0; kx < kernelW; ++kx) {
                            int iwNum = ox + padW - kx;
                            if (iwNum < 0) continue;
                            if ((iwNum % strideW) != 0) continue;
                            int iw = iwNum / strideW;
                            if (iw >= inW) continue;
                            // Note ic-outer weight layout: w[c, n, ky, kx]
                            float w = weight[((c * outChan + n) * kernelH + ky) * kernelW + kx];
                            float v = in[(c * inH + ih) * inW + iw];
                            acc += w * v;
                        }
                    }
                }
                out[(n * outH + oy) * outW + ox] = acc;
            }
        }
    }
}

bool runConv2DCpuSmoke(const Model& m) {
    // Find Conv_16 — first conv in encoder branch (3 RGB → 16 ch, k=3
    // s=2 p=1, has bias, LeakyReLU 0.2).
    QString convName;
    for (const auto& L : m.layers) {
        if (L.kind == OpKind::Convolution && L.name == "Conv_16") {
            convName = L.name; break;
        }
    }
    if (convName.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] CpuSmoke: Conv_16 not in graph (model layout changed?)");
        return false;
    }
    auto weight = getTensorAsFp32(m, "Conv_16/weight");
    auto bias   = getTensorAsFp32(m, "Conv_16/bias");
    if (weight.size() != 432 || bias.size() != 16) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] CpuSmoke: Conv_16 weight/bias size mismatch "
                     "(got %zu/%zu, expect 432/16)",
                     weight.size(), bias.size());
        return false;
    }

    // First fp16 weight from raw inspection was 0x28B3 = 0.036712.
    // Verify our unpack matches.
    const float kExpectedFirst = 0.036712f;
    float diff0 = std::abs(weight[0] - kExpectedFirst);
    if (diff0 > 1e-4f) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] CpuSmoke: fp16 unpack mismatch — got "
                     "weight[0]=%.6f, expect ~%.6f (delta=%.6f). "
                     "Either fp16 layout flag was misread, or our unpack has a bug.",
                     weight[0], kExpectedFirst, diff0);
        return false;
    }

    // Deterministic 64×64×3 RGB input via xorshift32 seeded on a
    // constant — so re-runs across machines / builds produce bit-identical
    // input and we can compare GPU output to CPU output later.
    constexpr int W = 64, H = 64, C = 3;
    constexpr int OUT_W = 32, OUT_H = 32, N = 16; // stride 2 pad 1 kernel 3
    std::vector<float> input((size_t)C * H * W);
    uint32_t st = 0x13371337u; // seed
    for (auto& v : input) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        v = (float)(st & 0xFFFFFF) / (float)0xFFFFFF;
    }
    std::vector<float> output((size_t)N * OUT_H * OUT_W);
    referenceConv2D(input.data(), W, H, C,
                    weight.data(), N, 3, 3,
                    bias.data(),
                    2, 2, 1, 1,
                    0.2f,
                    output.data(), OUT_W, OUT_H);

    // Sanity stats: no NaN, no Inf, plausible magnitudes.
    float lo = output[0], hi = output[0], sumAbs = 0.0f;
    int nanCount = 0, infCount = 0;
    for (float v : output) {
        if (std::isnan(v)) { nanCount++; continue; }
        if (std::isinf(v)) { infCount++; continue; }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sumAbs += std::abs(v);
    }
    float meanAbs = sumAbs / (float)output.size();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] CpuSmoke Conv_16: weight[0]=%.6f (✓ matches fp16 0x28B3) "
                "bias[0]=%.6f, output 16×32×32 lo=%.4f hi=%.4f mean|·|=%.4f "
                "(NaN=%d Inf=%d)",
                weight[0], bias[0], lo, hi, meanAbs, nanCount, infCount);
    bool pass = (nanCount == 0 && infCount == 0
                 && meanAbs > 1e-4f && meanAbs < 100.0f
                 && hi > lo);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] CpuSmoke verdict: %s",
                pass ? "PASS" : "FAIL");
    // Dump first 5 output values so we can later compare to GPU output.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] CpuSmoke output sample [0..4]: %.6f %.6f %.6f %.6f %.6f",
                output[0], output[1], output[2], output[3], output[4]);
    return pass;
}

// Helper: integer-typed param accessor with default.  ncnn writes ints
// without a decimal point so our parser stored them as Int.
static int64_t getInt(const Layer& L, int id, int64_t def = 0) {
    auto it = L.params.find(id);
    if (it == L.params.end()) return def;
    if (it->second.kind == ParamValue::Int)   return it->second.i;
    if (it->second.kind == ParamValue::Float) return (int64_t)it->second.f;
    return def;
}

bool loadWeights(const QString& binPath, Model& m) {
    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] loadWeights: cannot open %s",
                     qUtf8Printable(binPath));
        return false;
    }
    const qint64 fileSize = f.size();
    m.weightBlob.resize((size_t)fileSize);
    if (f.read(reinterpret_cast<char*>(m.weightBlob.data()), fileSize) != fileSize) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] loadWeights: short read from %s",
                     qUtf8Printable(binPath));
        m.weightBlob.clear();
        return false;
    }

    m.tensors.clear();
    m.tensorByName.clear();
    size_t cursor = 0;

    // ncnn fp16 magic: when a Convolution/Deconvolution weight tensor is
    // saved from ncnn2bin in fp16 mode, its block in .bin starts with this
    // 4-byte little-endian flag, then weight_data_size fp16 values.  Bias
    // tensors and MemoryData stay raw fp32 (no flag).  Verified empirically
    // for rife-v4.25-lite/flownet.bin (16128 fp32 MemoryData + 63 weight
    // tensors with fp16 flag + 63 fp32 biases = 11,276,252 bytes exactly).
    constexpr uint32_t kFp16Flag = 0x01306B47u;

    auto pushFp32Tensor = [&](WeightTensor&& wt) {
        wt.dtype     = TensorDType::Float32;
        wt.elemCount = (size_t)wt.n * (size_t)wt.c * (size_t)wt.h * (size_t)wt.w;
        wt.byteOffset = cursor;
        cursor += wt.elemCount * sizeof(float);
        m.tensorByName[wt.name] = (int)m.tensors.size();
        m.tensors.push_back(std::move(wt));
    };
    auto pushConvWeightTensor = [&](WeightTensor&& wt) -> bool {
        // Read the 4-byte flag first.
        if (cursor + 4 > m.weightBlob.size()) return false;
        uint32_t flag;
        std::memcpy(&flag, m.weightBlob.data() + cursor, sizeof(flag));
        cursor += 4;
        if (flag != kFp16Flag && flag != 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RIFE-VK] loadWeights: unexpected weight flag 0x%08X "
                        "for layer '%s' (expected 0x01306B47 fp16 or 0x00000000 fp32)",
                        flag, qUtf8Printable(wt.name));
        }
        wt.dtype     = (flag == kFp16Flag) ? TensorDType::Float16 : TensorDType::Float32;
        wt.elemCount = (size_t)wt.n * (size_t)wt.c * (size_t)wt.h * (size_t)wt.w;
        wt.byteOffset = cursor;
        size_t bytesPerElem = (wt.dtype == TensorDType::Float16) ? 2 : 4;
        cursor += wt.elemCount * bytesPerElem;
        m.tensorByName[wt.name] = (int)m.tensors.size();
        m.tensors.push_back(std::move(wt));
        return true;
    };

    for (int idx = 0; idx < (int)m.layers.size(); ++idx) {
        const Layer& L = m.layers[idx];
        if (cursor > (size_t)fileSize) break;

        if (L.kind == OpKind::MemoryData) {
            // ncnn MemoryData params: 0=w, 1=h, 2=c.  Default 0 → 1.
            int64_t w = std::max<int64_t>(1, getInt(L, 0, 1));
            int64_t h = std::max<int64_t>(1, getInt(L, 1, 1));
            int64_t c = std::max<int64_t>(1, getInt(L, 2, 1));
            WeightTensor wt;
            wt.name     = L.name;
            wt.layerIdx = idx;
            wt.n = (int)c; wt.c = 1; wt.h = (int)h; wt.w = (int)w;
            pushFp32Tensor(std::move(wt));
        } else if (L.kind == OpKind::Convolution || L.kind == OpKind::Deconvolution) {
            // ncnn Convolution params:
            //   0=num_output  1=kernel_w  (defaults: 11=k_w as 1)
            //   2=dilation_w  3=stride_w  4=pad_left  5=bias_term
            //   6=weight_data_size  9=activation_type
            int64_t numOut = getInt(L, 0, 0);
            int64_t kernel = getInt(L, 1, 1);
            int64_t weightSize = getInt(L, 6, 0);
            int64_t hasBias = getInt(L, 5, 0);
            if (numOut <= 0 || weightSize <= 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-RIFE-VK] loadWeights: layer '%s' has bad numOut=%lld "
                            "weightSize=%lld — skipping",
                            qUtf8Printable(L.name), (long long)numOut, (long long)weightSize);
                continue;
            }
            int64_t inChan = weightSize / (numOut * kernel * kernel);
            if (inChan * numOut * kernel * kernel != weightSize) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-RIFE-VK] loadWeights: layer '%s' weight_size=%lld doesn't "
                            "factor as numOut*kernel*kernel*in (numOut=%lld kernel=%lld) — using "
                            "raw size, dim layout will be wrong",
                            qUtf8Printable(L.name), (long long)weightSize,
                            (long long)numOut, (long long)kernel);
                inChan = 1;
                kernel = (int64_t)std::sqrt((double)(weightSize / numOut));
                if (kernel <= 0) kernel = 1;
            }
            WeightTensor wt;
            wt.name     = L.name + "/weight";
            wt.layerIdx = idx;
            wt.n = (int)numOut;
            wt.c = (int)inChan;
            wt.h = (int)kernel;
            wt.w = (int)kernel;
            // Conv/Deconv weights have an fp16 flag header; biases don't.
            if (!pushConvWeightTensor(std::move(wt))) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] loadWeights: ran out of bytes reading "
                             "weight flag for layer '%s'", qUtf8Printable(L.name));
                return false;
            }
            if (hasBias) {
                WeightTensor wb;
                wb.name     = L.name + "/bias";
                wb.layerIdx = idx;
                wb.n = (int)numOut;
                wb.c = 1; wb.h = 1; wb.w = 1;
                pushFp32Tensor(std::move(wb));
            }
        }
        // Other op kinds (Input, Split, BinaryOp, ReLU, Crop, Concat,
        // Interp, PixelShuffle, Sigmoid, Eltwise, RifeWarp, Unknown)
        // do not consume .bin bytes.
    }

    if (cursor != (size_t)fileSize) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] loadWeights: byte mismatch — expected %lld bytes "
                     "from %zu tensors, got file size %lld (delta=%lld)",
                     (long long)cursor, m.tensors.size(),
                     (long long)fileSize, (long long)fileSize - (long long)cursor);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] loadWeights OK: %zu tensors, %lld bytes (= .bin size)",
                m.tensors.size(), (long long)fileSize);
    return true;
}

// ============================================================================
// §J.3.e.X Phase 4g.1 — tensor shape inference.
//
// Given input shapes (in0/in1/in2), walks the layer graph in
// declaration order and computes (C, H, W) for every blob.  Returns
// false on any unknown op or shape rule failure.  Populates
// blobShapes (must be empty on entry).
//
// Per-op shape rules confirmed against rife-v4.25-lite/flownet.param
// (audit rounds in Phase 4a/4b/4c/4d/4e/4f), so this only covers what
// the model actually contains; future RIFE variants may need rule
// extensions (e.g. asymmetric padding, dilation > 1, Eltwise PROD).
// ============================================================================

bool inferBlobShapes(const Model& m,
                     const InferInputs& inputs,
                     std::unordered_map<QString, BlobShape>& blobShapes)
{
    // Map of blob name → shape.  Filled as we walk layers.
    auto get = [&](const QString& n, BlobShape& out) -> bool {
        auto it = blobShapes.find(n);
        if (it == blobShapes.end()) return false;
        out = it->second;
        return true;
    };

    // ncnn convolution kernel-extent helper accounting for dilation,
    // even though all 56 conv layers in this model are dilation=1.
    auto convKernelExtent = [](int k, int dilation) {
        return (k - 1) * dilation + 1;
    };

    int inputIdx = 0;  // assigns in0/in1/in2 in declaration order
    for (const Layer& L : m.layers) {
        switch (L.kind) {
        case OpKind::Input: {
            if (L.outputs.size() != 1) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] inferShapes: Input '%s' has %d outputs (expect 1)",
                             qUtf8Printable(L.name), (int)L.outputs.size());
                return false;
            }
            BlobShape s;
            if      (inputIdx == 0) s = inputs.in0;
            else if (inputIdx == 1) s = inputs.in1;
            else if (inputIdx == 2) s = inputs.in2;
            else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] inferShapes: more than 3 Input layers");
                return false;
            }
            if (!s.valid()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] inferShapes: Input %d shape invalid (c=%d h=%d w=%d)",
                             inputIdx, s.c, s.h, s.w);
                return false;
            }
            blobShapes[L.outputs[0]] = s;
            ++inputIdx;
            break;
        }
        case OpKind::MemoryData: {
            // params: 0=w, 1=h, 2=c (defaults 0).  In flownet all are
            // (w=1, h=1, c=N) per-channel beta scalars.
            int w = paramInt(L, 0, 0);
            int h = paramInt(L, 1, 1);
            int c = paramInt(L, 2, 1);
            if (h == 0) h = 1;
            if (c == 0) c = 1;
            if (w <= 0 || h <= 0 || c <= 0) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] inferShapes: MemoryData '%s' invalid wxhxc=%dx%dx%d",
                             qUtf8Printable(L.name), w, h, c);
                return false;
            }
            blobShapes[L.outputs[0]] = { c, h, w };
            break;
        }
        case OpKind::Split: {
            // Replicate input shape to all outputs.
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            for (const auto& o : L.outputs) blobShapes[o] = in;
            break;
        }
        case OpKind::Convolution: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            int outC   = paramInt(L, 0, 0);
            int kW     = paramInt(L, 1, 0);
            int kH     = paramInt(L, 11, kW);
            int dW     = paramInt(L, 2, 1);
            int dH     = paramInt(L, 12, dW);
            int sW     = paramInt(L, 3, 1);
            int sH     = paramInt(L, 13, sW);
            int padL   = paramInt(L, 4, 0);
            int padR   = paramInt(L, 14, padL);
            int padT   = paramInt(L, 15, padL);
            int padB   = paramInt(L, 16, padT);
            int kExtH  = convKernelExtent(kH, dH);
            int kExtW  = convKernelExtent(kW, dW);
            int outH   = (in.h + padT + padB - kExtH) / sH + 1;
            int outW   = (in.w + padL + padR - kExtW) / sW + 1;
            if (outC <= 0 || outH <= 0 || outW <= 0) return false;
            blobShapes[L.outputs[0]] = { outC, outH, outW };
            break;
        }
        case OpKind::Deconvolution: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            int outC = paramInt(L, 0, 0);
            int kW   = paramInt(L, 1, 0);
            int kH   = paramInt(L, 11, kW);
            int dW   = paramInt(L, 2, 1);
            int dH   = paramInt(L, 12, dW);
            int sW   = paramInt(L, 3, 1);
            int sH   = paramInt(L, 13, sW);
            int padL = paramInt(L, 4, 0);
            int padR = paramInt(L, 14, padL);
            int padT = paramInt(L, 15, padL);
            int padB = paramInt(L, 16, padT);
            int outPadR = paramInt(L, 18, 0);
            int outPadB = paramInt(L, 19, 0);
            int kExtH = convKernelExtent(kH, dH);
            int kExtW = convKernelExtent(kW, dW);
            int outH = (in.h - 1) * sH + kExtH - padT - padB + outPadB;
            int outW = (in.w - 1) * sW + kExtW - padL - padR + outPadR;
            // ncnn also has param 20/21 = absolute output size override.
            int absH = paramInt(L, 21, 0);
            int absW = paramInt(L, 20, 0);
            if (absH > 0) outH = absH;
            if (absW > 0) outW = absW;
            if (outC <= 0 || outH <= 0 || outW <= 0) return false;
            blobShapes[L.outputs[0]] = { outC, outH, outW };
            break;
        }
        case OpKind::BinaryOp: {
            // Output shape == input 0 (broadcast inputs match in this
            // model: tensor⊕tensor same-shape, tensor⊕beta channel-bcast,
            // tensor⊕scalar shape-preserving).
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            blobShapes[L.outputs[0]] = in;
            break;
        }
        case OpKind::ReLU:
        case OpKind::Sigmoid: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            blobShapes[L.outputs[0]] = in;
            break;
        }
        case OpKind::Crop: {
            // Audit: all 45 crops slice along channel axis only.
            // -23311=axes (1, 0)  -23309=starts  -23310=ends
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            auto itEnd = L.params.find(-23310);
            auto itStart = L.params.find(-23309);
            if (itEnd == L.params.end() || itStart == L.params.end()
                || itEnd->second.ia.size() < 1 || itStart->second.ia.size() < 1) {
                return false;
            }
            int s = (int)itStart->second.ia[0];
            int e = (int)itEnd->second.ia[0];
            if (e == 2147483647) e = in.c;
            int outC = e - s;
            if (outC <= 0) return false;
            blobShapes[L.outputs[0]] = { outC, in.h, in.w };
            break;
        }
        case OpKind::Concat: {
            // Default axis 0 = channel for ncnn 3D blobs.
            int outC = 0;
            int outH = 0, outW = 0;
            for (int i = 0; i < (int)L.inputs.size(); ++i) {
                BlobShape in;
                if (!get(L.inputs[i], in)) return false;
                outC += in.c;
                if (i == 0) { outH = in.h; outW = in.w; }
            }
            blobShapes[L.outputs[0]] = { outC, outH, outW };
            break;
        }
        case OpKind::Interp: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            // params: 1=height_scale, 2=width_scale, 3=output_height,
            // 4=output_width.  3 layers in flownet have only 0=2 set
            // (no scale, no output dim) → identity.
            float hScale = 1.0f, wScale = 1.0f;
            auto itHs = L.params.find(1);
            auto itWs = L.params.find(2);
            if (itHs != L.params.end()) hScale = (float)itHs->second.f;
            if (itWs != L.params.end()) wScale = (float)itWs->second.f;
            int outH = paramInt(L, 3, 0);
            int outW = paramInt(L, 4, 0);
            if (outH == 0) outH = (int)((float)in.h * hScale + 0.5f);
            if (outW == 0) outW = (int)((float)in.w * wScale + 0.5f);
            if (outH <= 0) outH = in.h;
            if (outW <= 0) outW = in.w;
            blobShapes[L.outputs[0]] = { in.c, outH, outW };
            break;
        }
        case OpKind::PixelShuffle: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            int r = paramInt(L, 0, 1);
            if (r <= 0 || in.c % (r * r) != 0) return false;
            blobShapes[L.outputs[0]] = { in.c / (r * r), in.h * r, in.w * r };
            break;
        }
        case OpKind::Eltwise: {
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            blobShapes[L.outputs[0]] = in;
            break;
        }
        case OpKind::RifeWarp: {
            // Output shape == image (input 0); flow is input 1 and has
            // a different shape (2, H, W).
            BlobShape in;
            if (!get(L.inputs[0], in)) return false;
            blobShapes[L.outputs[0]] = in;
            break;
        }
        case OpKind::Unknown:
        default:
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] inferShapes: unknown op kind for layer '%s' (%s)",
                         qUtf8Printable(L.name), qUtf8Printable(L.opType));
            return false;
        }

        // Sanity: every output we just assigned must be valid.
        for (const auto& o : L.outputs) {
            auto it = blobShapes.find(o);
            if (it == blobShapes.end() || !it->second.valid()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] inferShapes: layer '%s' output '%s' invalid",
                             qUtf8Printable(L.name), qUtf8Printable(o));
                return false;
            }
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] inferShapes: %zu blob shapes computed (model claims blobCount=%d)",
                blobShapes.size(), m.blobCount);
    return true;
}

void dumpModelSmoke(const QString& modelDir) {
    QString paramPath = modelDir + "/flownet.param";
    QString binPath   = modelDir + "/flownet.bin";

    QFileInfo binInfo(binPath);
    if (!binInfo.exists()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] flownet.bin missing at %s",
                     qUtf8Printable(binPath));
        return;
    }

    Model m;
    if (!parseParam(paramPath, m)) return;

    // Build sorted op-kind summary string.
    QString opSummary;
    auto add = [&](OpKind k) {
        auto it = m.opCounts.find(k);
        if (it == m.opCounts.end()) return;
        opSummary += QString(" %1=%2").arg(opKindName(k)).arg(it->second);
    };
    add(OpKind::Input);  add(OpKind::Convolution);  add(OpKind::Deconvolution);
    add(OpKind::BinaryOp); add(OpKind::Split); add(OpKind::ReLU);
    add(OpKind::MemoryData); add(OpKind::Crop); add(OpKind::Concat);
    add(OpKind::Interp); add(OpKind::PixelShuffle); add(OpKind::Sigmoid);
    add(OpKind::Eltwise); add(OpKind::RifeWarp); add(OpKind::Unknown);

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] §J.3.e.X scaffold: parsed %s (magic=%d "
                "layerCount=%d blobCount=%d, .bin=%lld B)",
                qUtf8Printable(paramPath), m.magic, m.layerCount,
                m.blobCount, (long long)binInfo.size());
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] op distribution:%s",
                qUtf8Printable(opSummary));

    // Dump head + tail layers (terminal i/o tensors give us a sanity
    // check that input names are in0/in1/in2 and output is out0).
    auto layerLine = [](const Layer& L) -> QString {
        return QString("  [%1] %2 in=[%3] out=[%4]")
            .arg(opKindName(L.kind))
            .arg(L.name)
            .arg(L.inputs.join(','))
            .arg(L.outputs.join(','));
    };
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RIFE-VK] head 3 layers:");
    for (int i = 0; i < (int)m.layers.size() && i < 3; ++i) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s",
                    qUtf8Printable(layerLine(m.layers[i])));
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RIFE-VK] tail 3 layers:");
    for (int i = std::max(0, (int)m.layers.size() - 3); i < (int)m.layers.size(); ++i) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "%s",
                    qUtf8Printable(layerLine(m.layers[i])));
    }

    // §J.3.e.X Phase 2 — load weights and verify total bytes match.
    // Failure here signals .param/.bin out-of-sync (different model build).
    if (!loadWeights(binPath, m)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] weight load FAILED — Phase 2 won't proceed");
        return;
    }
    // Sample a few tensors so we can spot-check sizes line up with .param.
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RIFE-VK] first 3 weight tensors:");
    for (int i = 0; i < (int)m.tensors.size() && i < 3; ++i) {
        const auto& t = m.tensors[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "  '%s' n=%d c=%d h=%d w=%d %s (%zu elems @ +%zu)",
                    qUtf8Printable(t.name), t.n, t.c, t.h, t.w,
                    t.dtype == TensorDType::Float16 ? "fp16" : "fp32",
                    t.elemCount, t.byteOffset);
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RIFE-VK] last 3 weight tensors:");
    for (int i = std::max(0, (int)m.tensors.size() - 3); i < (int)m.tensors.size(); ++i) {
        const auto& t = m.tensors[i];
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "  '%s' n=%d c=%d h=%d w=%d %s (%zu elems @ +%zu)",
                    qUtf8Printable(t.name), t.n, t.c, t.h, t.w,
                    t.dtype == TensorDType::Float16 ? "fp16" : "fp32",
                    t.elemCount, t.byteOffset);
    }
    // Spot-check first MemoryData tensor's first 3 fp32 values against the
    // hex inspection (0.022, 0.103, 0.053) to confirm raw-fp32 layout.
    if (!m.tensors.empty()) {
        const auto& t0 = m.tensors[0];
        if (t0.byteOffset + 3 * sizeof(float) <= m.weightBlob.size()) {
            const float* p = reinterpret_cast<const float*>(m.weightBlob.data() + t0.byteOffset);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-RIFE-VK] tensor[0] '%s' first 3 floats: %.6f %.6f %.6f "
                        "(expect ~0.022 ~0.103 ~0.053 from raw .bin inspection)",
                        qUtf8Printable(t0.name), p[0], p[1], p[2]);
        }
    }

    // §J.3.e.X Phase 3a — CPU reference Conv2D smoke against real
    // Conv_16 weights.  Verifies fp16 unpack + Conv math both correct
    // before we sink time into Vulkan dispatch infrastructure.
    runConv2DCpuSmoke(m);
}

// ============================================================================
// §J.3.e.X Phase 3b.2 — Vulkan GPU compute test
//
// Runs Conv_16 on the GPU via the kConv2DShaderGlsl compute shader,
// reads back the output, and compares to the trusted CPU reference
// (referenceConv2D from Phase 3a).  Self-contained: builds its own
// VkShaderModule / VkDescriptorSetLayout / VkPipeline / VkBuffers /
// VkCommandPool against caller-supplied VkDevice, dispatches once,
// verifies, tears everything down.
//
// Mimics the buildPipeline lambda pattern in vkfruc.cpp's
// createFrucComputeResources for consistency with existing VkFrucRenderer
// compute infrastructure.
// ============================================================================

namespace {

uint32_t findHostVisibleMemoryType(VkPhysicalDeviceMemoryProperties& mp,
                                    uint32_t typeBits) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i))) continue;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                   | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

struct GpuBuffer {
    VkBuffer        buf = VK_NULL_HANDLE;
    VkDeviceMemory  mem = VK_NULL_HANDLE;
    void*           mapped = nullptr;
    size_t          size = 0;
};

bool createHostBuffer(VkDevice device,
                      VkPhysicalDeviceMemoryProperties& mp,
                      PFN_vkCreateBuffer pfnCreateBuffer,
                      PFN_vkGetBufferMemoryRequirements pfnGetReq,
                      PFN_vkAllocateMemory pfnAlloc,
                      PFN_vkBindBufferMemory pfnBind,
                      PFN_vkMapMemory pfnMap,
                      VkDeviceSize size, GpuBuffer& out)
{
    out.size = (size_t)size;
    VkBufferCreateInfo bci = {};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size  = size;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
              | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (pfnCreateBuffer(device, &bci, nullptr, &out.buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr = {};
    pfnGetReq(device, out.buf, &mr);
    uint32_t typeIdx = findHostVisibleMemoryType(mp, mr.memoryTypeBits);
    if (typeIdx == UINT32_MAX) return false;
    VkMemoryAllocateInfo mai = {};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = typeIdx;
    if (pfnAlloc(device, &mai, nullptr, &out.mem) != VK_SUCCESS) return false;
    if (pfnBind(device, out.buf, out.mem, 0) != VK_SUCCESS) return false;
    if (pfnMap(device, out.mem, 0, VK_WHOLE_SIZE, 0, &out.mapped) != VK_SUCCESS) return false;
    return true;
}

void destroyHostBuffer(VkDevice device,
                       PFN_vkUnmapMemory pfnUnmap,
                       PFN_vkDestroyBuffer pfnDestroyBuf,
                       PFN_vkFreeMemory pfnFreeMem,
                       GpuBuffer& b)
{
    if (b.mapped && pfnUnmap)        pfnUnmap(device, b.mem);
    if (b.buf  && pfnDestroyBuf)     pfnDestroyBuf(device, b.buf, nullptr);
    if (b.mem  && pfnFreeMem)        pfnFreeMem(device, b.mem, nullptr);
    b = {};
}

// ============================================================================
// §J.3.e.X Phase 4d helper — runComputeOnce.
// Wraps the full "compile shader → build pipeline → alloc buffers →
// dispatch → read back → tear down" boilerplate that Phase 3b/4a/4b/4c
// repeated inline.  New per-op tests (Phase 4d+) build on this so the
// per-op delta stays small.
// ============================================================================

struct ComputeBufferSpec {
    const float* hostInData = nullptr;  // null = leave zero-initialised
    size_t       elemCount  = 0;        // bytes = elemCount * sizeof(float); >0 required
    float*       readbackOut = nullptr; // null = skip readback for this binding
};

struct RunComputeOptions {
    const char*       shaderGlsl   = nullptr;
    int               bindingCount = 0;
    ComputeBufferSpec buffers[8]   = {};
    const void*       pcData       = nullptr;
    size_t            pcSize       = 0;
    uint32_t          dispX        = 1;
    uint32_t          dispY        = 1;
    uint32_t          dispZ        = 1;
};

bool runComputeOnce(const VulkanCtx& ctx, const RunComputeOptions& opts) {
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto pd       = (VkPhysicalDevice)ctx.physicalDevice;
    auto device   = (VkDevice)ctx.device;
    auto queue    = (VkQueue)ctx.computeQueue;
    if (!pfnGetInstancePA || !instance || !pd || !device || !queue) return false;
    if (opts.bindingCount <= 0 || opts.bindingCount > 8) return false;

    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction {
        return pfnGetDevPa(device, n);
    };
#define LOAD(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) return false;
    LOAD(CreateShaderModule)        LOAD(DestroyShaderModule)
    LOAD(CreateDescriptorSetLayout) LOAD(DestroyDescriptorSetLayout)
    LOAD(CreatePipelineLayout)      LOAD(DestroyPipelineLayout)
    LOAD(CreateComputePipelines)    LOAD(DestroyPipeline)
    LOAD(CreateBuffer)              LOAD(DestroyBuffer)
    LOAD(GetBufferMemoryRequirements)
    LOAD(AllocateMemory)            LOAD(FreeMemory)
    LOAD(BindBufferMemory)
    LOAD(MapMemory)                 LOAD(UnmapMemory)
    LOAD(CreateDescriptorPool)      LOAD(DestroyDescriptorPool)
    LOAD(AllocateDescriptorSets)    LOAD(UpdateDescriptorSets)
    LOAD(CreateCommandPool)         LOAD(DestroyCommandPool)
    LOAD(AllocateCommandBuffers)
    LOAD(BeginCommandBuffer)        LOAD(EndCommandBuffer)
    LOAD(CmdBindPipeline)           LOAD(CmdBindDescriptorSets)
    LOAD(CmdPushConstants)          LOAD(CmdDispatch)
    LOAD(QueueSubmit)
    LOAD(CreateFence)               LOAD(DestroyFence)
    LOAD(WaitForFences)
#undef LOAD
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)pfnGetInstancePA(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnGetPdMemProps) return false;
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(pd, &memProps);

    std::vector<uint32_t> spirv;
    {
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(opts.shaderGlsl, opt, spirv) != 0
            || spirv.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] runComputeOnce: compile_spirv_module failed");
            return false;
        }
    }

    VkShaderModule shaderMod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) return false;
    }

    VkDescriptorSetLayoutBinding dslB[8] = {};
    for (int i = 0; i < opts.bindingCount; ++i) {
        dslB[i].binding = (uint32_t)i;
        dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dslB[i].descriptorCount = 1;
        dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = (uint32_t)opts.bindingCount;
        dslCi.pBindings = dslB;
        if (pfnCreateDescriptorSetLayout(device, &dslCi, nullptr, &dsl) != VK_SUCCESS) {
            pfnDestroyShaderModule(device, shaderMod, nullptr);
            return false;
        }
    }

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    // Round to 16-byte alignment.
    size_t pcSizeAligned = (opts.pcSize + 15) & ~size_t(15);
    pcRange.size = (uint32_t)pcSizeAligned;
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &dsl;
        plCi.pushConstantRangeCount = (opts.pcSize > 0) ? 1 : 0;
        plCi.pPushConstantRanges    = (opts.pcSize > 0) ? &pcRange : nullptr;
        if (pfnCreatePipelineLayout(device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) {
            pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
            pfnDestroyShaderModule(device, shaderMod, nullptr);
            return false;
        }
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = shaderMod;
        cpCi.stage.pName = "main";
        cpCi.layout = pipeLay;
        if (pfnCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipeline) != VK_SUCCESS) {
            pfnDestroyPipelineLayout(device, pipeLay, nullptr);
            pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
            pfnDestroyShaderModule(device, shaderMod, nullptr);
            return false;
        }
    }

    GpuBuffer bufs[8] = {};
    bool ok = true;
    for (int i = 0; i < opts.bindingCount; ++i) {
        size_t bytes = opts.buffers[i].elemCount * sizeof(float);
        if (bytes == 0) bytes = sizeof(float);  // empty buffer placeholder
        if (!createHostBuffer(device, memProps,
                              pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                              pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                              bytes, bufs[i])) { ok = false; break; }
        if (opts.buffers[i].hostInData) {
            std::memcpy(bufs[i].mapped, opts.buffers[i].hostInData,
                        opts.buffers[i].elemCount * sizeof(float));
        } else {
            std::memset(bufs[i].mapped, 0, bufs[i].size);
        }
    }
    if (!ok) {
        for (int i = 0; i < 8; ++i)
            destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufs[i]);
        pfnDestroyPipeline(device, pipeline, nullptr);
        pfnDestroyPipelineLayout(device, pipeLay, nullptr);
        pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
        pfnDestroyShaderModule(device, shaderMod, nullptr);
        return false;
    }

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sz = {};
        sz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sz.descriptorCount = (uint32_t)opts.bindingCount;
        VkDescriptorPoolCreateInfo dpCi = {};
        dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCi.maxSets = 1;
        dpCi.poolSizeCount = 1;
        dpCi.pPoolSizes = &sz;
        pfnCreateDescriptorPool(device, &dpCi, nullptr, &descPool);
    }
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        pfnAllocateDescriptorSets(device, &dsai, &descSet);
    }
    {
        VkDescriptorBufferInfo dbi[8] = {};
        VkWriteDescriptorSet  wds[8] = {};
        for (int i = 0; i < opts.bindingCount; ++i) {
            dbi[i].buffer = bufs[i].buf;
            dbi[i].range  = VK_WHOLE_SIZE;
            wds[i].sType  = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = descSet;
            wds[i].dstBinding = (uint32_t)i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &dbi[i];
        }
        pfnUpdateDescriptorSets(device, (uint32_t)opts.bindingCount, wds, 0, nullptr);
    }

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpCi.queueFamilyIndex = ctx.computeQueueFamily;
        cpCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pfnCreateCommandPool(device, &cpCi, nullptr, &cmdPool);
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        pfnAllocateCommandBuffers(device, &cbai, &cmd);
    }
    {
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        pfnBeginCommandBuffer(cmd, &cbbi);
    }
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    pfnCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLay, 0,
                             1, &descSet, 0, nullptr);
    if (opts.pcSize > 0) {
        pfnCmdPushConstants(cmd, pipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                            (uint32_t)opts.pcSize, opts.pcData);
    }
    pfnCmdDispatch(cmd, opts.dispX, opts.dispY, opts.dispZ);
    pfnEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        pfnCreateFence(device, &fci, nullptr, &fence);
    }
    VkResult vrSubmit;
    {
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vrSubmit = pfnQueueSubmit(queue, 1, &si, fence);
    }
    if (vrSubmit == VK_SUCCESS) {
        pfnWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        // Read back any bindings that asked for it.
        for (int i = 0; i < opts.bindingCount; ++i) {
            if (opts.buffers[i].readbackOut) {
                std::memcpy(opts.buffers[i].readbackOut, bufs[i].mapped,
                            opts.buffers[i].elemCount * sizeof(float));
            }
        }
    }

    pfnDestroyFence(device, fence, nullptr);
    pfnDestroyCommandPool(device, cmdPool, nullptr);
    for (int i = 0; i < opts.bindingCount; ++i) {
        destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufs[i]);
    }
    pfnDestroyDescriptorPool(device, descPool, nullptr);
    pfnDestroyPipeline(device, pipeline, nullptr);
    pfnDestroyPipelineLayout(device, pipeLay, nullptr);
    pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
    pfnDestroyShaderModule(device, shaderMod, nullptr);
    return vrSubmit == VK_SUCCESS;
}

// ============================================================================
// §J.3.e.X Phase 4g.2 — persistent pipeline cache.
//
// runComputeOnce (above) compiles shader → builds pipeline → dispatches
// → tears down on every call: fine for the per-op correctness gates
// where we run each shader twice or three times, but unworkable for
// the graph executor that dispatches 389 times PER FRAME.  The cache
// holds 9 cached pipelines (one per ShaderKind) plus their layouts /
// descriptor set layouts, built once at executor init.
//
// 4g.2 deliverable: build all 9 pipelines, verify all created without
// VK errors, tear down cleanly.  4g.3+ promotes the same data
// structures to executor-lifetime ownership.
// ============================================================================

enum class ShaderKind : int {
    Conv2D = 0,
    Deconv2D,
    BinaryOp,
    Activation,
    Copy,
    PixelShuffle,
    InterpBilinear,
    EltwiseSum,
    RifeWarp,
    Count
};

struct ShaderSpec {
    const char* name;
    const char* (*sourceFn)();
    int         bindingCount;
    uint32_t    pushConstantBytes;  // total push constant range size (already 16-byte aligned)
};

// Per-shader spec.  bindingCount + pushConstantBytes must match the
// GLSL declarations + the C++ structs in runConv2DGpuTest /
// runBinaryOpGpuTest / etc.  Drift here = pipeline build succeeds but
// dispatches go to wrong descriptor slots.
static const ShaderSpec kShaderSpecs[(int)ShaderKind::Count] = {
    { "Conv2D",         getConv2DShaderGlsl,         4, 64 },
    { "Deconv2D",       getDeconv2DShaderGlsl,       4, 48 },
    { "BinaryOp",       getBinaryOpShaderGlsl,       3, 32 },
    { "Activation",     getActivationShaderGlsl,     2, 16 },
    { "Copy",           getCopyShaderGlsl,           2, 16 },
    { "PixelShuffle",   getPixelShuffleShaderGlsl,   2, 16 },
    { "InterpBilinear", getInterpBilinearShaderGlsl, 2, 32 },
    { "EltwiseSum",     getEltwiseShaderGlsl,        3, 16 },
    { "RifeWarp",       getRifeWarpShaderGlsl,       3, 16 },
};

struct CachedPipeline {
    VkShaderModule        module   = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl      = VK_NULL_HANDLE;
    VkPipelineLayout      layout   = VK_NULL_HANDLE;
    VkPipeline            pipeline = VK_NULL_HANDLE;
    int                   bindings = 0;
};

struct PipelineCache {
    VkDevice       device = VK_NULL_HANDLE;
    CachedPipeline pipelines[(int)ShaderKind::Count] = {};
    // Destroyer PFNs cached so cleanup doesn't re-load.
    PFN_vkDestroyShaderModule        pfnDestroyShader = nullptr;
    PFN_vkDestroyDescriptorSetLayout pfnDestroyDSL    = nullptr;
    PFN_vkDestroyPipelineLayout      pfnDestroyPL     = nullptr;
    PFN_vkDestroyPipeline            pfnDestroyPipe   = nullptr;
};

void destroyPipelineCache(PipelineCache& c) {
    if (c.device == VK_NULL_HANDLE) return;
    for (int i = 0; i < (int)ShaderKind::Count; ++i) {
        auto& cp = c.pipelines[i];
        if (cp.pipeline && c.pfnDestroyPipe) c.pfnDestroyPipe(c.device, cp.pipeline, nullptr);
        if (cp.layout   && c.pfnDestroyPL)   c.pfnDestroyPL  (c.device, cp.layout,   nullptr);
        if (cp.dsl      && c.pfnDestroyDSL)  c.pfnDestroyDSL (c.device, cp.dsl,      nullptr);
        if (cp.module   && c.pfnDestroyShader) c.pfnDestroyShader(c.device, cp.module, nullptr);
        cp = {};
    }
    c.device = VK_NULL_HANDLE;
}

bool buildPipelineCache(const VulkanCtx& ctx, PipelineCache& out) {
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto device   = (VkDevice)ctx.device;
    if (!pfnGetInstancePA || !instance || !device) return false;
    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction { return pfnGetDevPa(device, n); };

#define LOAD(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) return false;
    LOAD(CreateShaderModule)        LOAD(DestroyShaderModule)
    LOAD(CreateDescriptorSetLayout) LOAD(DestroyDescriptorSetLayout)
    LOAD(CreatePipelineLayout)      LOAD(DestroyPipelineLayout)
    LOAD(CreateComputePipelines)    LOAD(DestroyPipeline)
#undef LOAD

    out.device           = device;
    out.pfnDestroyShader = pfnDestroyShaderModule;
    out.pfnDestroyDSL    = pfnDestroyDescriptorSetLayout;
    out.pfnDestroyPL     = pfnDestroyPipelineLayout;
    out.pfnDestroyPipe   = pfnDestroyPipeline;

    for (int i = 0; i < (int)ShaderKind::Count; ++i) {
        const ShaderSpec& spec = kShaderSpecs[i];
        CachedPipeline& cp = out.pipelines[i];

        // 1. Compile GLSL → SPIR-V (ncnn glslang).
        std::vector<uint32_t> spirv;
        {
            ncnn::Option opt;
            if (ncnn::compile_spirv_module(spec.sourceFn(), opt, spirv) != 0
                || spirv.empty()) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[VIPLE-RIFE-VK] PipelineCache: %s compile failed",
                             spec.name);
                destroyPipelineCache(out);
                return false;
            }
        }

        // 2. VkShaderModule
        {
            VkShaderModuleCreateInfo smCi = {};
            smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            smCi.codeSize = spirv.size() * sizeof(uint32_t);
            smCi.pCode = spirv.data();
            if (pfnCreateShaderModule(device, &smCi, nullptr, &cp.module) != VK_SUCCESS) {
                destroyPipelineCache(out);
                return false;
            }
        }

        // 3. VkDescriptorSetLayout (N storage buffers)
        cp.bindings = spec.bindingCount;
        {
            VkDescriptorSetLayoutBinding dslB[8] = {};
            for (int b = 0; b < spec.bindingCount; ++b) {
                dslB[b].binding = (uint32_t)b;
                dslB[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                dslB[b].descriptorCount = 1;
                dslB[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dslCi = {};
            dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            dslCi.bindingCount = (uint32_t)spec.bindingCount;
            dslCi.pBindings = dslB;
            if (pfnCreateDescriptorSetLayout(device, &dslCi, nullptr, &cp.dsl) != VK_SUCCESS) {
                destroyPipelineCache(out);
                return false;
            }
        }

        // 4. VkPipelineLayout (push constant range)
        {
            VkPushConstantRange pcRange = {};
            pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            pcRange.size = spec.pushConstantBytes;
            VkPipelineLayoutCreateInfo plCi = {};
            plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            plCi.setLayoutCount = 1;
            plCi.pSetLayouts = &cp.dsl;
            plCi.pushConstantRangeCount = (spec.pushConstantBytes > 0) ? 1 : 0;
            plCi.pPushConstantRanges    = (spec.pushConstantBytes > 0) ? &pcRange : nullptr;
            if (pfnCreatePipelineLayout(device, &plCi, nullptr, &cp.layout) != VK_SUCCESS) {
                destroyPipelineCache(out);
                return false;
            }
        }

        // 5. VkPipeline
        {
            VkComputePipelineCreateInfo cpCi = {};
            cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpCi.stage.module = cp.module;
            cpCi.stage.pName = "main";
            cpCi.layout = cp.layout;
            if (pfnCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &cp.pipeline) != VK_SUCCESS) {
                destroyPipelineCache(out);
                return false;
            }
        }
    }
    return true;
}

} // anonymous namespace

bool runPipelineCacheSmoke(const VulkanCtx& ctx) {
    PipelineCache cache;
    if (!buildPipelineCache(ctx, cache)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] PipelineCacheSmoke: build FAILED");
        std::fprintf(stderr,
            "[VIPLE-RIFE-VK] PipelineCacheSmoke: build FAILED\n");
        std::fflush(stderr);
        return false;
    }
    int built = 0;
    for (int i = 0; i < (int)ShaderKind::Count; ++i) {
        if (cache.pipelines[i].pipeline != VK_NULL_HANDLE) ++built;
    }
    bool pass = (built == (int)ShaderKind::Count);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] PipelineCacheSmoke: %d/%d pipelines built — %s",
                built, (int)ShaderKind::Count, pass ? "PASS" : "FAIL");
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] PipelineCacheSmoke: %d/%d pipelines built — %s\n",
        built, (int)ShaderKind::Count, pass ? "PASS" : "FAIL");
    std::fflush(stderr);
    destroyPipelineCache(cache);
    return pass;
}

// ============================================================================
// §J.3.e.X Phase 4g.3 — buffer pool + MemoryData / weight prefill.
//
// One host-visible mapped buffer per blob (485 entries) and one per
// weight/bias tensor (~118 entries).  Buffers are sized via the
// Phase 4g.1 shape inference output for blobs, and via WeightTensor
// elemCount for tensor buffers.  MemoryData blobs are filled from the
// .bin's pre-loaded fp32 array; Convolution / Deconvolution weight +
// bias buffers are filled from getTensorAsFp32 (handles fp16 unpack).
//
// Production executor will use device-local buffers + staging.  For
// correctness gates at 256×256 (≈ tens of MB total) host-visible is
// fast enough and simpler.
// ============================================================================

namespace {

struct BufferPool {
    VkDevice                                  device   = VK_NULL_HANDLE;
    PFN_vkUnmapMemory                         pfnUnmap = nullptr;
    PFN_vkDestroyBuffer                       pfnDestroyBuf = nullptr;
    PFN_vkFreeMemory                          pfnFreeMem = nullptr;
    std::unordered_map<QString, GpuBuffer>    blobs;
    std::unordered_map<QString, GpuBuffer>    tensors;
};

void destroyBufferPool(BufferPool& p) {
    if (p.device == VK_NULL_HANDLE) return;
    for (auto& kv : p.blobs)   destroyHostBuffer(p.device, p.pfnUnmap, p.pfnDestroyBuf, p.pfnFreeMem, kv.second);
    for (auto& kv : p.tensors) destroyHostBuffer(p.device, p.pfnUnmap, p.pfnDestroyBuf, p.pfnFreeMem, kv.second);
    p.blobs.clear();
    p.tensors.clear();
    p.device = VK_NULL_HANDLE;
}

bool buildBufferPool(const VulkanCtx& ctx,
                     const Model& m,
                     const std::unordered_map<QString, BlobShape>& shapes,
                     BufferPool& out)
{
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto pd       = (VkPhysicalDevice)ctx.physicalDevice;
    auto device   = (VkDevice)ctx.device;
    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction { return pfnGetDevPa(device, n); };
#define LOAD(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) return false;
    LOAD(CreateBuffer)              LOAD(DestroyBuffer)
    LOAD(GetBufferMemoryRequirements)
    LOAD(AllocateMemory)            LOAD(FreeMemory)
    LOAD(BindBufferMemory)
    LOAD(MapMemory)                 LOAD(UnmapMemory)
#undef LOAD
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)pfnGetInstancePA(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnGetPdMemProps) return false;
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(pd, &memProps);

    out.device        = device;
    out.pfnUnmap      = pfnUnmapMemory;
    out.pfnDestroyBuf = pfnDestroyBuffer;
    out.pfnFreeMem    = pfnFreeMemory;

    // ---- Blob buffers (one per inferred shape) ----
    for (const auto& kv : shapes) {
        const QString& name = kv.first;
        const BlobShape& s = kv.second;
        size_t bytes = (size_t)s.c * s.h * s.w * sizeof(float);
        if (bytes == 0) bytes = sizeof(float);
        GpuBuffer buf{};
        if (!createHostBuffer(device, memProps,
                              pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                              pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                              bytes, buf)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] BufferPool: blob '%s' alloc failed (%zu B)",
                         qUtf8Printable(name), bytes);
            destroyBufferPool(out);
            return false;
        }
        std::memset(buf.mapped, 0, buf.size);
        out.blobs.emplace(name, buf);
    }

    // ---- Weight/bias tensor buffers (Conv / Deconv / MemoryData) ----
    for (const auto& t : m.tensors) {
        size_t bytes = t.elemCount * sizeof(float);
        if (bytes == 0) bytes = sizeof(float);
        GpuBuffer buf{};
        if (!createHostBuffer(device, memProps,
                              pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                              pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                              bytes, buf)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] BufferPool: tensor '%s' alloc failed (%zu B)",
                         qUtf8Printable(t.name), bytes);
            destroyBufferPool(out);
            return false;
        }
        // Pre-fill from .bin (handles fp16 unpack for Conv weights).
        std::vector<float> fp32 = getTensorAsFp32(m, t.name);
        if (!fp32.empty() && fp32.size() * sizeof(float) <= buf.size) {
            std::memcpy(buf.mapped, fp32.data(), fp32.size() * sizeof(float));
        }
        out.tensors.emplace(t.name, buf);
    }

    // ---- For MemoryData: also seed the matching BLOB buffer with the
    //      same fp32 data (the layer's "output blob" name == its
    //      tensor name, so dispatch handlers can read either; but
    //      Phase 4g.4 will treat MemoryData as a no-op layer that
    //      just relies on the blob being pre-filled).
    for (const Layer& L : m.layers) {
        if (L.kind != OpKind::MemoryData) continue;
        if (L.outputs.isEmpty()) continue;
        const QString& blobName = L.outputs[0];
        // The MemoryData blob name == the tensor name in this model.
        std::vector<float> fp32 = getTensorAsFp32(m, blobName);
        auto blobIt = out.blobs.find(blobName);
        if (blobIt != out.blobs.end() && !fp32.empty()
            && fp32.size() * sizeof(float) <= blobIt->second.size) {
            std::memcpy(blobIt->second.mapped,
                        fp32.data(),
                        fp32.size() * sizeof(float));
        }
    }
    return true;
}

} // namespace

bool runBlobBufferPoolSmoke(const VulkanCtx& ctx, const QString& modelDir) {
    Model m;
    if (!parseParam(modelDir + "/flownet.param", m)
        || !loadWeights(modelDir + "/flownet.bin", m)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] BufferPoolSmoke: model load failed");
        return false;
    }

    InferInputs in{};
    in.in0 = { 3, 256, 256 };
    in.in1 = { 3, 256, 256 };
    in.in2 = { 1,   1,   1 };
    std::unordered_map<QString, BlobShape> shapes;
    if (!inferBlobShapes(m, in, shapes)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] BufferPoolSmoke: shape inference failed");
        return false;
    }

    BufferPool pool;
    if (!buildBufferPool(ctx, m, shapes, pool)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] BufferPoolSmoke: pool build failed");
        return false;
    }

    bool pass = true;
    if ((int)pool.blobs.size() != m.blobCount) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] BufferPoolSmoke: blob count %zu != %d",
                     pool.blobs.size(), m.blobCount);
        pass = false;
    }
    if (pool.tensors.size() != m.tensors.size()) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] BufferPoolSmoke: tensor count %zu != %zu",
                     pool.tensors.size(), m.tensors.size());
        pass = false;
    }

    // Spot check: MemoryData blob 'block0.convblock.0.beta' first 3 fp32
    // should be ~0.022, ~0.103, ~0.053 (matches dumpModelSmoke's Phase 2
    // anchor from raw .bin inspection).
    auto it = pool.blobs.find("block0.convblock.0.beta");
    if (it == pool.blobs.end()) {
        std::fprintf(stderr,
            "[VIPLE-RIFE-VK] BufferPoolSmoke: MemoryData blob missing ✗\n");
        pass = false;
    } else {
        const float* p = (const float*)it->second.mapped;
        std::fprintf(stderr,
            "[VIPLE-RIFE-VK] BufferPoolSmoke: 'block0.convblock.0.beta' first 3 = "
            "%.6f %.6f %.6f (expect ~0.022 ~0.103 ~0.053)\n",
            (double)p[0], (double)p[1], (double)p[2]);
        // Anchor: matches the raw .bin first 3 fp32 from dumpModelSmoke.
        if (!(std::abs(p[0] - 0.022f) < 0.01f
              && std::abs(p[1] - 0.103f) < 0.01f
              && std::abs(p[2] - 0.053f) < 0.01f)) {
            std::fprintf(stderr,
                "[VIPLE-RIFE-VK] BufferPoolSmoke: anchor mismatch ✗\n");
            pass = false;
        }
    }

    // Spot check: Conv_16/weight first value should match Phase 3a's
    // anchor (fp16 0x28B3 → ~0.036713).
    auto wit = pool.tensors.find("Conv_16/weight");
    if (wit == pool.tensors.end()) {
        std::fprintf(stderr,
            "[VIPLE-RIFE-VK] BufferPoolSmoke: Conv_16/weight tensor missing ✗\n");
        pass = false;
    } else {
        const float* p = (const float*)wit->second.mapped;
        std::fprintf(stderr,
            "[VIPLE-RIFE-VK] BufferPoolSmoke: 'Conv_16/weight' [0] = %.6f (expect ~0.036713)\n",
            (double)p[0]);
        if (std::abs(p[0] - 0.036713f) > 0.001f) {
            std::fprintf(stderr,
                "[VIPLE-RIFE-VK] BufferPoolSmoke: weight anchor mismatch ✗\n");
            pass = false;
        }
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] BufferPoolSmoke: %zu blobs + %zu tensors — %s",
                pool.blobs.size(), pool.tensors.size(), pass ? "PASS" : "FAIL");
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] BufferPoolSmoke: %zu blobs + %zu tensors — %s\n",
        pool.blobs.size(), pool.tensors.size(), pass ? "PASS" : "FAIL");
    std::fflush(stderr);

    destroyBufferPool(pool);
    return pass;
}

bool runConv2DGpuTest(const VulkanCtx& ctx,
                     const Model& m,
                     const QString& layerName,
                     float tolerance) {
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto pd       = (VkPhysicalDevice)ctx.physicalDevice;
    auto device   = (VkDevice)ctx.device;
    auto queue    = (VkQueue)ctx.computeQueue;
    if (!pfnGetInstancePA || !instance || !pd || !device || !queue) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] GpuTest: VulkanCtx incomplete; skipping");
        return false;
    }

    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction {
        return pfnGetDevPa(device, n);
    };

#define LOAD_PFN(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) { \
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-RIFE-VK] GpuTest: missing vk" #NAME); \
            return false; \
        }
    LOAD_PFN(CreateShaderModule)
    LOAD_PFN(DestroyShaderModule)
    LOAD_PFN(CreateDescriptorSetLayout)
    LOAD_PFN(DestroyDescriptorSetLayout)
    LOAD_PFN(CreatePipelineLayout)
    LOAD_PFN(DestroyPipelineLayout)
    LOAD_PFN(CreateComputePipelines)
    LOAD_PFN(DestroyPipeline)
    LOAD_PFN(CreateBuffer)
    LOAD_PFN(DestroyBuffer)
    LOAD_PFN(GetBufferMemoryRequirements)
    LOAD_PFN(AllocateMemory)
    LOAD_PFN(FreeMemory)
    LOAD_PFN(BindBufferMemory)
    LOAD_PFN(MapMemory)
    LOAD_PFN(UnmapMemory)
    LOAD_PFN(CreateDescriptorPool)
    LOAD_PFN(DestroyDescriptorPool)
    LOAD_PFN(AllocateDescriptorSets)
    LOAD_PFN(UpdateDescriptorSets)
    LOAD_PFN(CreateCommandPool)
    LOAD_PFN(DestroyCommandPool)
    LOAD_PFN(AllocateCommandBuffers)
    LOAD_PFN(BeginCommandBuffer)
    LOAD_PFN(EndCommandBuffer)
    LOAD_PFN(CmdBindPipeline)
    LOAD_PFN(CmdBindDescriptorSets)
    LOAD_PFN(CmdPushConstants)
    LOAD_PFN(CmdDispatch)
    LOAD_PFN(QueueSubmit)
    LOAD_PFN(QueueWaitIdle)
    LOAD_PFN(CreateFence)
    LOAD_PFN(DestroyFence)
    LOAD_PFN(WaitForFences)
#undef LOAD_PFN
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)pfnGetInstancePA(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnGetPdMemProps) return false;
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(pd, &memProps);

    // ---- 1. Look up the layer + extract its conv hyperparameters ----
    const Layer* L = findLayerByName(m, layerName);
    if (!L || L->kind != OpKind::Convolution) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] GpuTest: layer '%s' not found or not Convolution",
                     qUtf8Printable(layerName));
        return false;
    }
    const int N        = paramInt(*L, 0, 0);          // num_output
    const int kW       = paramInt(*L, 1, 0);
    const int kH       = paramInt(*L, 11, kW);
    const int strideW  = paramInt(*L, 3, 1);
    const int strideH  = paramInt(*L, 13, strideW);
    const int pad      = paramInt(*L, 4, 0);
    const int hasBias  = paramInt(*L, 5, 0);
    const int wsize    = paramInt(*L, 6, 0);
    const float lrSlope = leakyReluSlopeOf(*L);
    if (N <= 0 || kW <= 0 || kH <= 0 || wsize <= 0 || N * kH * kW <= 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] GpuTest %s: invalid conv params (N=%d kHkW=%dx%d wsize=%d)",
                     qUtf8Printable(layerName), N, kH, kW, wsize);
        return false;
    }
    const int C = wsize / (N * kH * kW);  // input channels
    auto weight = getTensorAsFp32(m, layerName + "/weight");
    auto bias   = hasBias ? getTensorAsFp32(m, layerName + "/bias")
                          : std::vector<float>{};
    if ((int)weight.size() != wsize) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] GpuTest %s: weight size %zu != param wsize %d",
                     qUtf8Printable(layerName), weight.size(), wsize);
        return false;
    }
    if (hasBias && (int)bias.size() != N) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] GpuTest %s: bias size %zu != N %d",
                     qUtf8Printable(layerName), bias.size(), N);
        return false;
    }

    // ---- 2. Generate deterministic 64×64×C input ----
    constexpr int W = 64, H = 64;
    const int OUT_W = (W + 2 * pad - kW) / strideW + 1;
    const int OUT_H = (H + 2 * pad - kH) / strideH + 1;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] GpuTest %s: in=%dx%dx%d → out=%dx%dx%d "
                "(k=%dx%d s=%dx%d p=%d bias=%d lr=%.2f)",
                qUtf8Printable(layerName), W, H, C, OUT_W, OUT_H, N,
                kW, kH, strideW, strideH, pad, hasBias, (double)lrSlope);
    std::vector<float> input((size_t)C * H * W);
    uint32_t st = 0x13371337u;
    for (auto& v : input) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        v = (float)(st & 0xFFFFFF) / (float)0xFFFFFF;
    }
    std::vector<float> cpuOut((size_t)N * OUT_H * OUT_W);
    referenceConv2D(input.data(), W, H, C,
                    weight.data(), N, kH, kW,
                    hasBias ? bias.data() : nullptr,
                    strideH, strideW, pad, pad,
                    lrSlope,
                    cpuOut.data(), OUT_W, OUT_H);

    // ---- 3. Compile shader to SPIR-V ----
    std::vector<uint32_t> spirv;
    {
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(getConv2DShaderGlsl(), opt, spirv) != 0
            || spirv.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] GpuTest: compile_spirv_module failed "
                         "(ncnn::create_gpu_instance not called?)");
            return false;
        }
    }

    // ---- 4. Build pipeline + buffers ----
    VkShaderModule shaderMod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) return false;
    }

    VkDescriptorSetLayoutBinding dslB[4] = {};
    for (int i = 0; i < 4; ++i) {
        dslB[i].binding = (uint32_t)i;
        dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dslB[i].descriptorCount = 1;
        dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = 4;
        dslCi.pBindings = dslB;
        if (pfnCreateDescriptorSetLayout(device, &dslCi, nullptr, &dsl) != VK_SUCCESS) return false;
    }

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size = 13 * sizeof(int32_t); // 3 ivec4 + 1 float, but pad to 16-byte boundary
    if (pcRange.size < 64) pcRange.size = 64;
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &dsl;
        plCi.pushConstantRangeCount = 1;
        plCi.pPushConstantRanges = &pcRange;
        if (pfnCreatePipelineLayout(device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) return false;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = shaderMod;
        cpCi.stage.pName = "main";
        cpCi.layout = pipeLay;
        if (pfnCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipeline) != VK_SUCCESS) return false;
    }

    GpuBuffer bufIn = {}, bufW = {}, bufB = {}, bufOut = {};
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          input.size() * sizeof(float), bufIn)) return false;
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          weight.size() * sizeof(float), bufW)) return false;
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          bias.size() * sizeof(float), bufB)) return false;
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          cpuOut.size() * sizeof(float), bufOut)) return false;
    std::memcpy(bufIn.mapped,  input.data(),  bufIn.size);
    std::memcpy(bufW.mapped,   weight.data(), bufW.size);
    std::memcpy(bufB.mapped,   bias.data(),   bufB.size);
    std::memset(bufOut.mapped, 0,             bufOut.size);

    // ---- 5. Descriptor set ----
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sz = {};
        sz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sz.descriptorCount = 4;
        VkDescriptorPoolCreateInfo dpCi = {};
        dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCi.maxSets = 1;
        dpCi.poolSizeCount = 1;
        dpCi.pPoolSizes = &sz;
        if (pfnCreateDescriptorPool(device, &dpCi, nullptr, &descPool) != VK_SUCCESS) return false;
    }
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        if (pfnAllocateDescriptorSets(device, &dsai, &descSet) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorBufferInfo dbi[4] = {};
        dbi[0].buffer = bufIn.buf;  dbi[0].range = VK_WHOLE_SIZE;
        dbi[1].buffer = bufW.buf;   dbi[1].range = VK_WHOLE_SIZE;
        dbi[2].buffer = bufB.buf;   dbi[2].range = VK_WHOLE_SIZE;
        dbi[3].buffer = bufOut.buf; dbi[3].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet wds[4] = {};
        for (int i = 0; i < 4; ++i) {
            wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = descSet;
            wds[i].dstBinding = (uint32_t)i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &dbi[i];
        }
        pfnUpdateDescriptorSets(device, 4, wds, 0, nullptr);
    }

    // ---- 6. Command buffer + dispatch ----
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpCi.queueFamilyIndex = ctx.computeQueueFamily;
        cpCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (pfnCreateCommandPool(device, &cpCi, nullptr, &cmdPool) != VK_SUCCESS) return false;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (pfnAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) return false;
    }
    {
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (pfnBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) return false;
    }
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    pfnCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLay, 0,
                             1, &descSet, 0, nullptr);
    // Push constants: 3 ivec4 + 1 float, packed to 13 ints = 52 bytes
    // (we allocated 64 to satisfy 16-byte alignment).
    struct PC {
        int32_t inDims[4];   // (inW, inH, inC, hasBias)
        int32_t outDims[4];  // (outW, outH, outChan, _pad)
        int32_t conv[4];     // (kernelW, kernelH, stride, pad)
        float   leakyReluSlope;
        int32_t _pad[3];     // round to 16
    } pcVal = {};
    pcVal.inDims[0] = W; pcVal.inDims[1] = H; pcVal.inDims[2] = C; pcVal.inDims[3] = hasBias;
    pcVal.outDims[0] = OUT_W; pcVal.outDims[1] = OUT_H; pcVal.outDims[2] = N;
    // Note: shader's `conv` ivec4 is (kernelW, kernelH, stride, pad) and
    // assumes square stride / pad — verified by Phase 4a audit (all 56
    // Conv layers in flownet are kH==kW, strideH==strideW, symmetric pad).
    pcVal.conv[0] = kW; pcVal.conv[1] = kH; pcVal.conv[2] = strideW; pcVal.conv[3] = pad;
    pcVal.leakyReluSlope = lrSlope;
    pfnCmdPushConstants(cmd, pipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(pcVal), &pcVal);
    pfnCmdDispatch(cmd, (OUT_W + 7) / 8, (OUT_H + 7) / 8, N);
    pfnEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (pfnCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) return false;
    }
    {
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkResult vr = pfnQueueSubmit(queue, 1, &si, fence);
        if (vr != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] GpuTest: vkQueueSubmit failed %d", (int)vr);
            return false;
        }
    }
    pfnWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    // ---- 7. Read GPU output, compare to CPU reference ----
    const float* gpuOut = reinterpret_cast<const float*>(bufOut.mapped);
    float maxAbsErr = 0.0f;
    int worstIdx = 0;
    for (size_t i = 0; i < cpuOut.size(); ++i) {
        float diff = std::abs(gpuOut[i] - cpuOut[i]);
        if (diff > maxAbsErr) { maxAbsErr = diff; worstIdx = (int)i; }
    }
    bool pass = maxAbsErr <= tolerance;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] GpuTest %s: max_abs_err=%.6e (worst idx=%d "
                "cpu=%.6f gpu=%.6f), tolerance=%.6e — %s",
                qUtf8Printable(layerName),
                (double)maxAbsErr, worstIdx,
                (double)cpuOut[worstIdx], (double)gpuOut[worstIdx],
                (double)tolerance, pass ? "PASS" : "FAIL");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] GpuTest %s output sample [0..4]: %.6f %.6f %.6f %.6f %.6f",
                qUtf8Printable(layerName),
                (double)gpuOut[0], (double)gpuOut[1], (double)gpuOut[2],
                (double)gpuOut[3], (double)gpuOut[4]);
    // SDL_Log on Windows can route to OutputDebugString instead of stderr
    // before our log handler is wired up.  fprintf gives the standalone
    // self-test (which runs pre-handler-install) a visible result line.
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] GpuTest %s: max_abs_err=%.6e (worst idx=%d "
        "cpu=%.6f gpu=%.6f), tolerance=%.6e — %s\n"
        "[VIPLE-RIFE-VK] GpuTest %s output sample [0..4]: %.6f %.6f %.6f %.6f %.6f\n",
        qUtf8Printable(layerName),
        (double)maxAbsErr, worstIdx,
        (double)cpuOut[worstIdx], (double)gpuOut[worstIdx],
        (double)tolerance, pass ? "PASS" : "FAIL",
        qUtf8Printable(layerName),
        (double)gpuOut[0], (double)gpuOut[1], (double)gpuOut[2],
        (double)gpuOut[3], (double)gpuOut[4]);
    std::fflush(stderr);

    // ---- 8. Cleanup ----
    pfnDestroyFence(device, fence, nullptr);
    pfnDestroyCommandPool(device, cmdPool, nullptr);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufIn);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufW);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufB);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufOut);
    pfnDestroyDescriptorPool(device, descPool, nullptr);
    pfnDestroyPipeline(device, pipeline, nullptr);
    pfnDestroyPipelineLayout(device, pipeLay, nullptr);
    pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
    pfnDestroyShaderModule(device, shaderMod, nullptr);
    return pass;
}

// ============================================================================
// §J.3.e.X Phase 4b — BinaryOp CPU reference + GPU correctness gate.
// Self-contained from any model file; takes raw input arrays so the
// shader gets exercised independently of layer wiring.
// ============================================================================

void referenceBinaryOp(const BinaryOpDesc& desc,
                       const float* a, size_t aCount,
                       const float* b,
                       float* out)
{
    auto apply = [op = desc.opType](float aa, float bb) -> float {
        switch (op) {
            case 0: return aa + bb;
            case 1: return aa - bb;
            case 2: return aa * bb;
            case 3: return aa / bb;
            case 4: return std::max(aa, bb);
            case 5: return std::min(aa, bb);
            case 6: return std::pow(aa, bb);
            case 7: return bb - aa;          // RSUB
            case 8: return bb / aa;          // RDIV
            case 9: return std::pow(bb, aa); // RPOW
            default: return aa;
        }
    };
    if (desc.mode == 2) {
        const float bb = desc.scalarB;
        for (size_t i = 0; i < aCount; ++i) out[i] = apply(a[i], bb);
    } else if (desc.mode == 1) {
        const size_t hw = (size_t)desc.hw;
        const size_t C  = (size_t)desc.channels;
        for (size_t i = 0; i < aCount; ++i) {
            size_t ch = (i / hw) % C;
            out[i] = apply(a[i], b[ch]);
        }
    } else {
        for (size_t i = 0; i < aCount; ++i) out[i] = apply(a[i], b[i]);
    }
}

bool runBinaryOpGpuTest(const VulkanCtx& ctx,
                        const BinaryOpDesc& desc,
                        const std::vector<float>& a,
                        const std::vector<float>& b,
                        float tolerance)
{
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto pd       = (VkPhysicalDevice)ctx.physicalDevice;
    auto device   = (VkDevice)ctx.device;
    auto queue    = (VkQueue)ctx.computeQueue;
    if (!pfnGetInstancePA || !instance || !pd || !device || !queue) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-RIFE-VK] BinOpTest: VulkanCtx incomplete; skipping");
        return false;
    }

    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction {
        return pfnGetDevPa(device, n);
    };
#define LOAD_PFN(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) { return false; }
    LOAD_PFN(CreateShaderModule)
    LOAD_PFN(DestroyShaderModule)
    LOAD_PFN(CreateDescriptorSetLayout)
    LOAD_PFN(DestroyDescriptorSetLayout)
    LOAD_PFN(CreatePipelineLayout)
    LOAD_PFN(DestroyPipelineLayout)
    LOAD_PFN(CreateComputePipelines)
    LOAD_PFN(DestroyPipeline)
    LOAD_PFN(CreateBuffer)
    LOAD_PFN(DestroyBuffer)
    LOAD_PFN(GetBufferMemoryRequirements)
    LOAD_PFN(AllocateMemory)
    LOAD_PFN(FreeMemory)
    LOAD_PFN(BindBufferMemory)
    LOAD_PFN(MapMemory)
    LOAD_PFN(UnmapMemory)
    LOAD_PFN(CreateDescriptorPool)
    LOAD_PFN(DestroyDescriptorPool)
    LOAD_PFN(AllocateDescriptorSets)
    LOAD_PFN(UpdateDescriptorSets)
    LOAD_PFN(CreateCommandPool)
    LOAD_PFN(DestroyCommandPool)
    LOAD_PFN(AllocateCommandBuffers)
    LOAD_PFN(BeginCommandBuffer)
    LOAD_PFN(EndCommandBuffer)
    LOAD_PFN(CmdBindPipeline)
    LOAD_PFN(CmdBindDescriptorSets)
    LOAD_PFN(CmdPushConstants)
    LOAD_PFN(CmdDispatch)
    LOAD_PFN(QueueSubmit)
    LOAD_PFN(CreateFence)
    LOAD_PFN(DestroyFence)
    LOAD_PFN(WaitForFences)
#undef LOAD_PFN
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)pfnGetInstancePA(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnGetPdMemProps) return false;
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(pd, &memProps);

    // ---- 1. CPU reference ----
    std::vector<float> cpuOut(a.size());
    referenceBinaryOp(desc, a.data(), a.size(),
                      desc.mode == 2 ? nullptr : b.data(),
                      cpuOut.data());

    // ---- 2. Compile shader ----
    std::vector<uint32_t> spirv;
    {
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(getBinaryOpShaderGlsl(), opt, spirv) != 0
            || spirv.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] BinOpTest: compile_spirv_module failed");
            return false;
        }
    }

    VkShaderModule shaderMod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) return false;
    }

    // 3 storage buffer bindings: a, b, out
    VkDescriptorSetLayoutBinding dslB[3] = {};
    for (int i = 0; i < 3; ++i) {
        dslB[i].binding = (uint32_t)i;
        dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dslB[i].descriptorCount = 1;
        dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = 3;
        dslCi.pBindings = dslB;
        if (pfnCreateDescriptorSetLayout(device, &dslCi, nullptr, &dsl) != VK_SUCCESS) return false;
    }

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size = 32;  // 6 fields × 4 bytes = 24, round up to 32
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &dsl;
        plCi.pushConstantRangeCount = 1;
        plCi.pPushConstantRanges = &pcRange;
        if (pfnCreatePipelineLayout(device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) return false;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = shaderMod;
        cpCi.stage.pName = "main";
        cpCi.layout = pipeLay;
        if (pfnCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipeline) != VK_SUCCESS) return false;
    }

    // ---- 3. Buffers ----
    GpuBuffer bufA = {}, bufB = {}, bufOut = {};
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          a.size() * sizeof(float), bufA)) return false;
    // For mode==2 (scalar) we still need a non-zero buffer for binding
    // 1; allocate a 1-float dummy.  Otherwise size from b.
    size_t bBytes = (desc.mode == 2 ? sizeof(float) : b.size() * sizeof(float));
    if (bBytes == 0) bBytes = sizeof(float);
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          bBytes, bufB)) return false;
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          a.size() * sizeof(float), bufOut)) return false;
    std::memcpy(bufA.mapped, a.data(), bufA.size);
    if (desc.mode != 2 && !b.empty()) {
        std::memcpy(bufB.mapped, b.data(), b.size() * sizeof(float));
    } else {
        float dummy = 0.0f;
        std::memcpy(bufB.mapped, &dummy, sizeof(float));
    }
    std::memset(bufOut.mapped, 0, bufOut.size);

    // ---- 4. Descriptor set ----
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sz = {};
        sz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sz.descriptorCount = 3;
        VkDescriptorPoolCreateInfo dpCi = {};
        dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCi.maxSets = 1;
        dpCi.poolSizeCount = 1;
        dpCi.pPoolSizes = &sz;
        if (pfnCreateDescriptorPool(device, &dpCi, nullptr, &descPool) != VK_SUCCESS) return false;
    }
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        if (pfnAllocateDescriptorSets(device, &dsai, &descSet) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorBufferInfo dbi[3] = {};
        dbi[0].buffer = bufA.buf;   dbi[0].range = VK_WHOLE_SIZE;
        dbi[1].buffer = bufB.buf;   dbi[1].range = VK_WHOLE_SIZE;
        dbi[2].buffer = bufOut.buf; dbi[2].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet wds[3] = {};
        for (int i = 0; i < 3; ++i) {
            wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = descSet;
            wds[i].dstBinding = (uint32_t)i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &dbi[i];
        }
        pfnUpdateDescriptorSets(device, 3, wds, 0, nullptr);
    }

    // ---- 5. Command buffer ----
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpCi.queueFamilyIndex = ctx.computeQueueFamily;
        cpCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (pfnCreateCommandPool(device, &cpCi, nullptr, &cmdPool) != VK_SUCCESS) return false;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (pfnAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) return false;
    }
    {
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (pfnBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) return false;
    }
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    pfnCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLay, 0,
                             1, &descSet, 0, nullptr);
    struct PC {
        uint32_t count;
        uint32_t hw;
        uint32_t channels;
        int32_t  opType;
        int32_t  mode;
        float    scalarB;
        uint32_t _pad[2];   // round to 32 bytes for alignment safety
    } pcVal = {};
    pcVal.count    = (uint32_t)a.size();
    pcVal.hw       = (uint32_t)desc.hw;
    pcVal.channels = (uint32_t)desc.channels;
    pcVal.opType   = desc.opType;
    pcVal.mode     = desc.mode;
    pcVal.scalarB  = desc.scalarB;
    pfnCmdPushConstants(cmd, pipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(pcVal), &pcVal);
    uint32_t groups = (uint32_t)((a.size() + 63) / 64);
    pfnCmdDispatch(cmd, groups, 1, 1);
    pfnEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (pfnCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) return false;
    }
    {
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (pfnQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
    }
    pfnWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    // ---- 6. Compare ----
    const float* gpuOut = reinterpret_cast<const float*>(bufOut.mapped);
    float maxAbsErr = 0.0f;
    int worstIdx = 0;
    for (size_t i = 0; i < cpuOut.size(); ++i) {
        float diff = std::abs(gpuOut[i] - cpuOut[i]);
        if (diff > maxAbsErr) { maxAbsErr = diff; worstIdx = (int)i; }
    }
    bool pass = maxAbsErr <= tolerance;
    const char* opNames[] = { "ADD","SUB","MUL","DIV","MAX","MIN","POW","RSUB","RDIV","RPOW" };
    const char* opName = (desc.opType >= 0 && desc.opType < 10) ? opNames[desc.opType] : "?";
    const char* modeNames[] = { "same", "chan-bcast", "scalar" };
    const char* modeName = (desc.mode >= 0 && desc.mode < 3) ? modeNames[desc.mode] : "?";
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] BinOpTest %s/%s (count=%zu c=%d hw=%d): "
                "max_abs_err=%.6e — %s",
                opName, modeName, a.size(), desc.channels, desc.hw,
                (double)maxAbsErr, pass ? "PASS" : "FAIL");
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] BinOpTest %s/%s (count=%zu c=%d hw=%d): "
        "max_abs_err=%.6e (worst idx=%d cpu=%.6f gpu=%.6f) — %s\n",
        opName, modeName, a.size(), desc.channels, desc.hw,
        (double)maxAbsErr, worstIdx,
        (double)cpuOut[worstIdx], (double)gpuOut[worstIdx],
        pass ? "PASS" : "FAIL");
    std::fflush(stderr);

    // ---- 7. Cleanup ----
    pfnDestroyFence(device, fence, nullptr);
    pfnDestroyCommandPool(device, cmdPool, nullptr);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufA);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufB);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufOut);
    pfnDestroyDescriptorPool(device, descPool, nullptr);
    pfnDestroyPipeline(device, pipeline, nullptr);
    pfnDestroyPipelineLayout(device, pipeLay, nullptr);
    pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
    pfnDestroyShaderModule(device, shaderMod, nullptr);
    return pass;
}

// ============================================================================
// §J.3.e.X Phase 4c — Activation CPU reference + GPU correctness gate.
// ============================================================================

void referenceActivation(const ActivationDesc& desc,
                         const float* a, size_t aCount,
                         float* out)
{
    if (desc.actType == 0) {
        const float s = desc.slope;
        for (size_t i = 0; i < aCount; ++i) out[i] = a[i] < 0.0f ? a[i] * s : a[i];
    } else if (desc.actType == 1) {
        for (size_t i = 0; i < aCount; ++i) out[i] = 1.0f / (1.0f + std::exp(-a[i]));
    } else {
        for (size_t i = 0; i < aCount; ++i) out[i] = a[i];
    }
}

bool runActivationGpuTest(const VulkanCtx& ctx,
                          const ActivationDesc& desc,
                          const std::vector<float>& a,
                          float tolerance)
{
    auto pfnGetInstancePA = (PFN_vkGetInstanceProcAddr)ctx.getInstanceProcAddr;
    auto instance = (VkInstance)ctx.instance;
    auto pd       = (VkPhysicalDevice)ctx.physicalDevice;
    auto device   = (VkDevice)ctx.device;
    auto queue    = (VkQueue)ctx.computeQueue;
    if (!pfnGetInstancePA || !instance || !pd || !device || !queue) return false;

    auto pfnGetDevPa = (PFN_vkGetDeviceProcAddr)pfnGetInstancePA(instance, "vkGetDeviceProcAddr");
    if (!pfnGetDevPa) return false;
    auto getDev = [&](const char* n) -> PFN_vkVoidFunction {
        return pfnGetDevPa(device, n);
    };
#define LOAD_PFN(NAME) auto pfn ## NAME = (PFN_vk ## NAME)getDev("vk" #NAME); \
        if (!pfn ## NAME) { return false; }
    LOAD_PFN(CreateShaderModule)        LOAD_PFN(DestroyShaderModule)
    LOAD_PFN(CreateDescriptorSetLayout) LOAD_PFN(DestroyDescriptorSetLayout)
    LOAD_PFN(CreatePipelineLayout)      LOAD_PFN(DestroyPipelineLayout)
    LOAD_PFN(CreateComputePipelines)    LOAD_PFN(DestroyPipeline)
    LOAD_PFN(CreateBuffer)              LOAD_PFN(DestroyBuffer)
    LOAD_PFN(GetBufferMemoryRequirements)
    LOAD_PFN(AllocateMemory)            LOAD_PFN(FreeMemory)
    LOAD_PFN(BindBufferMemory)
    LOAD_PFN(MapMemory)                 LOAD_PFN(UnmapMemory)
    LOAD_PFN(CreateDescriptorPool)      LOAD_PFN(DestroyDescriptorPool)
    LOAD_PFN(AllocateDescriptorSets)    LOAD_PFN(UpdateDescriptorSets)
    LOAD_PFN(CreateCommandPool)         LOAD_PFN(DestroyCommandPool)
    LOAD_PFN(AllocateCommandBuffers)
    LOAD_PFN(BeginCommandBuffer)        LOAD_PFN(EndCommandBuffer)
    LOAD_PFN(CmdBindPipeline)           LOAD_PFN(CmdBindDescriptorSets)
    LOAD_PFN(CmdPushConstants)          LOAD_PFN(CmdDispatch)
    LOAD_PFN(QueueSubmit)
    LOAD_PFN(CreateFence)               LOAD_PFN(DestroyFence)
    LOAD_PFN(WaitForFences)
#undef LOAD_PFN
    auto pfnGetPdMemProps = (PFN_vkGetPhysicalDeviceMemoryProperties)pfnGetInstancePA(
        instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!pfnGetPdMemProps) return false;
    VkPhysicalDeviceMemoryProperties memProps = {};
    pfnGetPdMemProps(pd, &memProps);

    std::vector<float> cpuOut(a.size());
    referenceActivation(desc, a.data(), a.size(), cpuOut.data());

    std::vector<uint32_t> spirv;
    {
        ncnn::Option opt;
        if (ncnn::compile_spirv_module(getActivationShaderGlsl(), opt, spirv) != 0
            || spirv.empty()) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] ActTest: compile_spirv_module failed");
            return false;
        }
    }

    VkShaderModule shaderMod = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smCi = {};
        smCi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smCi.codeSize = spirv.size() * sizeof(uint32_t);
        smCi.pCode = spirv.data();
        if (pfnCreateShaderModule(device, &smCi, nullptr, &shaderMod) != VK_SUCCESS) return false;
    }

    VkDescriptorSetLayoutBinding dslB[2] = {};
    for (int i = 0; i < 2; ++i) {
        dslB[i].binding = (uint32_t)i;
        dslB[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        dslB[i].descriptorCount = 1;
        dslB[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutCreateInfo dslCi = {};
        dslCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslCi.bindingCount = 2;
        dslCi.pBindings = dslB;
        if (pfnCreateDescriptorSetLayout(device, &dslCi, nullptr, &dsl) != VK_SUCCESS) return false;
    }

    VkPushConstantRange pcRange = {};
    pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcRange.size = 16;  // count (u32) + actType (i32) + slope (f32) = 12, round to 16
    VkPipelineLayout pipeLay = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo plCi = {};
        plCi.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &dsl;
        plCi.pushConstantRangeCount = 1;
        plCi.pPushConstantRanges = &pcRange;
        if (pfnCreatePipelineLayout(device, &plCi, nullptr, &pipeLay) != VK_SUCCESS) return false;
    }

    VkPipeline pipeline = VK_NULL_HANDLE;
    {
        VkComputePipelineCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpCi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpCi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpCi.stage.module = shaderMod;
        cpCi.stage.pName = "main";
        cpCi.layout = pipeLay;
        if (pfnCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &pipeline) != VK_SUCCESS) return false;
    }

    GpuBuffer bufIn = {}, bufOut = {};
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          a.size() * sizeof(float), bufIn)) return false;
    if (!createHostBuffer(device, memProps, pfnCreateBuffer, pfnGetBufferMemoryRequirements,
                          pfnAllocateMemory, pfnBindBufferMemory, pfnMapMemory,
                          a.size() * sizeof(float), bufOut)) return false;
    std::memcpy(bufIn.mapped, a.data(), bufIn.size);
    std::memset(bufOut.mapped, 0, bufOut.size);

    VkDescriptorPool descPool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sz = {};
        sz.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sz.descriptorCount = 2;
        VkDescriptorPoolCreateInfo dpCi = {};
        dpCi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpCi.maxSets = 1;
        dpCi.poolSizeCount = 1;
        dpCi.pPoolSizes = &sz;
        if (pfnCreateDescriptorPool(device, &dpCi, nullptr, &descPool) != VK_SUCCESS) return false;
    }
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = descPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &dsl;
        if (pfnAllocateDescriptorSets(device, &dsai, &descSet) != VK_SUCCESS) return false;
    }
    {
        VkDescriptorBufferInfo dbi[2] = {};
        dbi[0].buffer = bufIn.buf;  dbi[0].range = VK_WHOLE_SIZE;
        dbi[1].buffer = bufOut.buf; dbi[1].range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet wds[2] = {};
        for (int i = 0; i < 2; ++i) {
            wds[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds[i].dstSet = descSet;
            wds[i].dstBinding = (uint32_t)i;
            wds[i].descriptorCount = 1;
            wds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wds[i].pBufferInfo = &dbi[i];
        }
        pfnUpdateDescriptorSets(device, 2, wds, 0, nullptr);
    }

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo cpCi = {};
        cpCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpCi.queueFamilyIndex = ctx.computeQueueFamily;
        cpCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (pfnCreateCommandPool(device, &cpCi, nullptr, &cmdPool) != VK_SUCCESS) return false;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (pfnAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS) return false;
    }
    {
        VkCommandBufferBeginInfo cbbi = {};
        cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (pfnBeginCommandBuffer(cmd, &cbbi) != VK_SUCCESS) return false;
    }
    pfnCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    pfnCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLay, 0,
                             1, &descSet, 0, nullptr);
    struct PC {
        uint32_t count;
        int32_t  actType;
        float    slope;
        uint32_t _pad;
    } pcVal = {};
    pcVal.count   = (uint32_t)a.size();
    pcVal.actType = desc.actType;
    pcVal.slope   = desc.slope;
    pfnCmdPushConstants(cmd, pipeLay, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                        sizeof(pcVal), &pcVal);
    uint32_t groups = (uint32_t)((a.size() + 63) / 64);
    pfnCmdDispatch(cmd, groups, 1, 1);
    pfnEndCommandBuffer(cmd);

    VkFence fence = VK_NULL_HANDLE;
    {
        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (pfnCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) return false;
    }
    {
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        if (pfnQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
    }
    pfnWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    const float* gpuOut = reinterpret_cast<const float*>(bufOut.mapped);
    float maxAbsErr = 0.0f;
    int worstIdx = 0;
    for (size_t i = 0; i < cpuOut.size(); ++i) {
        float diff = std::abs(gpuOut[i] - cpuOut[i]);
        if (diff > maxAbsErr) { maxAbsErr = diff; worstIdx = (int)i; }
    }
    bool pass = maxAbsErr <= tolerance;
    const char* actName = (desc.actType == 0)
        ? (desc.slope == 0.0f ? "ReLU" : "LeakyReLU")
        : (desc.actType == 1 ? "Sigmoid" : "?");
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] ActTest %s slope=%.2f (count=%zu): "
                "max_abs_err=%.6e — %s",
                actName, (double)desc.slope, a.size(),
                (double)maxAbsErr, pass ? "PASS" : "FAIL");
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] ActTest %s slope=%.2f (count=%zu): "
        "max_abs_err=%.6e (worst idx=%d cpu=%.6f gpu=%.6f) — %s\n",
        actName, (double)desc.slope, a.size(),
        (double)maxAbsErr, worstIdx,
        (double)cpuOut[worstIdx], (double)gpuOut[worstIdx],
        pass ? "PASS" : "FAIL");
    std::fflush(stderr);

    pfnDestroyFence(device, fence, nullptr);
    pfnDestroyCommandPool(device, cmdPool, nullptr);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufIn);
    destroyHostBuffer(device, pfnUnmapMemory, pfnDestroyBuffer, pfnFreeMemory, bufOut);
    pfnDestroyDescriptorPool(device, descPool, nullptr);
    pfnDestroyPipeline(device, pipeline, nullptr);
    pfnDestroyPipelineLayout(device, pipeLay, nullptr);
    pfnDestroyDescriptorSetLayout(device, dsl, nullptr);
    pfnDestroyShaderModule(device, shaderMod, nullptr);
    return pass;
}

// ============================================================================
// §J.3.e.X Phase 4d — CPU references + GPU correctness gates for the 5
// shape ops.  Each test uses runComputeOnce (above) so the per-op delta
// is small (~50 LOC each).
// ============================================================================

void referenceCopy(const float* in, float* out,
                   size_t count, size_t srcOffset, size_t dstOffset)
{
    for (size_t i = 0; i < count; ++i) out[dstOffset + i] = in[srcOffset + i];
}

void referencePixelShuffle(const float* in, float* out,
                           int inC, int inH, int inW, int r)
{
    int outC = inC / (r * r);
    int outH = inH * r;
    int outW = inW * r;
    for (int oc = 0; oc < outC; ++oc) {
        for (int oh = 0; oh < outH; ++oh) {
            for (int ow = 0; ow < outW; ++ow) {
                int ic = oc * (r * r) + (oh % r) * r + (ow % r);
                int ih = oh / r;
                int iw = ow / r;
                out[(oc * outH + oh) * outW + ow] =
                    in[(ic * inH + ih) * inW + iw];
            }
        }
    }
}

void referenceInterpBilinear(const float* in, float* out,
                             int channels, int inH, int inW,
                             int outH, int outW)
{
    const float scaleH = (float)inH / (float)outH;
    const float scaleW = (float)inW / (float)outW;
    for (int c = 0; c < channels; ++c) {
        for (int oh = 0; oh < outH; ++oh) {
            for (int ow = 0; ow < outW; ++ow) {
                float inh_f = ((float)oh + 0.5f) * scaleH - 0.5f;
                float inw_f = ((float)ow + 0.5f) * scaleW - 0.5f;
                int ih0 = (int)std::floor(inh_f);
                int iw0 = (int)std::floor(inw_f);
                float fh = inh_f - (float)ih0;
                float fw = inw_f - (float)iw0;
                int ih1 = ih0 + 1;
                int iw1 = iw0 + 1;
                ih0 = std::max(0, std::min(ih0, inH - 1));
                ih1 = std::max(0, std::min(ih1, inH - 1));
                iw0 = std::max(0, std::min(iw0, inW - 1));
                iw1 = std::max(0, std::min(iw1, inW - 1));
                int planeBase = c * inH * inW;
                float v00 = in[planeBase + ih0 * inW + iw0];
                float v01 = in[planeBase + ih0 * inW + iw1];
                float v10 = in[planeBase + ih1 * inW + iw0];
                float v11 = in[planeBase + ih1 * inW + iw1];
                float v0  = v00 * (1.0f - fw) + v01 * fw;
                float v1  = v10 * (1.0f - fw) + v11 * fw;
                out[(c * outH + oh) * outW + ow]
                    = v0 * (1.0f - fh) + v1 * fh;
            }
        }
    }
}

void referenceEltwiseSum(const float* a, const float* b, float* out,
                         size_t count, float c0, float c1)
{
    for (size_t i = 0; i < count; ++i) out[i] = c0 * a[i] + c1 * b[i];
}

// Phase 4f — rife.Warp CPU reference (mirror of ncnn_rife_warp.cpp's
// CPU fallback at line 338-371; verbatim algorithm).
void referenceRifeWarp(const float* image, const float* flow,
                       float* out, int channels, int H, int W) {
    const int hw = H * W;
    const float* fxptr_base = flow;            // flow channel 0
    const float* fyptr_base = flow + hw;       // flow channel 1
    for (int q = 0; q < channels; ++q) {
        const float* imgPlane = image + (size_t)q * hw;
        float* outPlane = out + (size_t)q * hw;
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) {
                float fx = fxptr_base[y * W + x];
                float fy = fyptr_base[y * W + x];
                float sx = (float)x + fx;
                float sy = (float)y + fy;
                int x0 = (int)std::floor(sx);
                int y0 = (int)std::floor(sy);
                int x1 = x0 + 1, y1 = y0 + 1;
                x0 = std::max(0, std::min(x0, W - 1));
                y0 = std::max(0, std::min(y0, H - 1));
                x1 = std::max(0, std::min(x1, W - 1));
                y1 = std::max(0, std::min(y1, H - 1));
                float alpha = sx - (float)x0;
                float beta  = sy - (float)y0;
                float v0 = imgPlane[y0 * W + x0];
                float v1 = imgPlane[y0 * W + x1];
                float v2 = imgPlane[y1 * W + x0];
                float v3 = imgPlane[y1 * W + x1];
                float v4 = v0 * (1.0f - alpha) + v1 * alpha;
                float v5 = v2 * (1.0f - alpha) + v3 * alpha;
                outPlane[y * W + x] = v4 * (1.0f - beta) + v5 * beta;
            }
        }
    }
}

namespace {

// Generate `n` deterministic random floats centered on 0 with magnitude
// roughly 0.5.  Shared by the 5 shape-op tests so each call site stays
// short.
std::vector<float> deterministicInput(size_t n, uint32_t seed) {
    std::vector<float> v(n);
    uint32_t st = seed ? seed : 0x12345678u;
    for (auto& x : v) {
        st ^= st << 13; st ^= st >> 17; st ^= st << 5;
        x = (float)(st & 0xFFFFFF) / (float)0xFFFFFF - 0.5f;
    }
    return v;
}

void logOpResult(const char* label,
                 float maxErr, int worstIdx,
                 float cpuVal, float gpuVal,
                 float tolerance, bool pass)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] %s: max_abs_err=%.6e — %s",
                label, (double)maxErr, pass ? "PASS" : "FAIL");
    std::fprintf(stderr,
        "[VIPLE-RIFE-VK] %s: max_abs_err=%.6e (worst idx=%d cpu=%.6f gpu=%.6f) — %s\n",
        label, (double)maxErr, worstIdx,
        (double)cpuVal, (double)gpuVal, pass ? "PASS" : "FAIL");
    std::fflush(stderr);
}

float compareF32(const float* a, const float* b, size_t n, int* worstOut) {
    float maxErr = 0.0f; int wi = 0;
    for (size_t i = 0; i < n; ++i) {
        float d = std::abs(a[i] - b[i]);
        if (d > maxErr) { maxErr = d; wi = (int)i; }
    }
    if (worstOut) *worstOut = wi;
    return maxErr;
}

} // anonymous namespace

bool runCropGpuTest(const VulkanCtx& ctx,
                    int inC, int inH, int inW,
                    int cStart, int cEnd,
                    float tolerance)
{
    const int outC = cEnd - cStart;
    const size_t hw = (size_t)inH * inW;
    const size_t inCount  = (size_t)inC  * hw;
    const size_t outCount = (size_t)outC * hw;
    auto in = deterministicInput(inCount, 0xC0DEC0DEu);
    std::vector<float> cpuOut(outCount);
    referenceCopy(in.data(), cpuOut.data(), outCount, (size_t)cStart * hw, 0);

    std::vector<float> gpuOut(outCount, 0.0f);
    struct PC { uint32_t count, srcOff, dstOff; uint32_t _pad; } pc{};
    pc.count  = (uint32_t)outCount;
    pc.srcOff = (uint32_t)((size_t)cStart * hw);
    pc.dstOff = 0;

    RunComputeOptions opts{};
    opts.shaderGlsl   = getCopyShaderGlsl();
    opts.bindingCount = 2;
    opts.buffers[0]   = { in.data(),       inCount,  nullptr };
    opts.buffers[1]   = { nullptr,         outCount, gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((outCount + 63) / 64);
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), outCount, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "CropTest [%d:%d]/%d c=%dx%dx%d", cStart, cEnd, inC, outC, inH, inW);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runConcatGpuTest(const VulkanCtx& ctx,
                      int inA_C, int inB_C, int H, int W,
                      float tolerance)
{
    const size_t hw = (size_t)H * W;
    const size_t aCount = (size_t)inA_C * hw;
    const size_t bCount = (size_t)inB_C * hw;
    const size_t outCount = aCount + bCount;
    auto inA = deterministicInput(aCount, 0xAAAA1234u);
    auto inB = deterministicInput(bCount, 0xBBBB5678u);
    std::vector<float> cpuOut(outCount);
    referenceCopy(inA.data(), cpuOut.data(), aCount, 0, 0);
    referenceCopy(inB.data(), cpuOut.data(), bCount, 0, aCount);

    // Two dispatches, each writing one input into output.  We need to
    // re-use runComputeOnce twice — but the helper allocates new
    // buffers each call.  For correctness gate purposes that's fine;
    // the graph executor will manage shared buffers across dispatches.
    std::vector<float> gpuOut(outCount, 0.0f);

    auto dispatchOne = [&](const std::vector<float>& src,
                           size_t dstOffset) -> bool {
        struct PC { uint32_t count, srcOff, dstOff; uint32_t _pad; } pc{};
        pc.count  = (uint32_t)src.size();
        pc.srcOff = 0;
        pc.dstOff = (uint32_t)dstOffset;

        // To verify Concat we need to *compose* the result across two
        // dispatches: feed `gpuOut`-so-far as the output of pass 2 (so
        // pass 2's writes leave pass 1's writes untouched).  The
        // helper takes hostInData for buffer 1 as initial values.
        RunComputeOptions opts{};
        opts.shaderGlsl   = getCopyShaderGlsl();
        opts.bindingCount = 2;
        opts.buffers[0]   = { src.data(), src.size(), nullptr };
        opts.buffers[1]   = { gpuOut.data(), outCount, gpuOut.data() };
        opts.pcData       = &pc;
        opts.pcSize       = sizeof(pc);
        opts.dispX        = (uint32_t)((src.size() + 63) / 64);
        return runComputeOnce(ctx, opts);
    };
    if (!dispatchOne(inA, 0))      return false;
    if (!dispatchOne(inB, aCount)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), outCount, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "ConcatTest %d+%d=%dch hw=%dx%d", inA_C, inB_C, inA_C+inB_C, H, W);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runPixelShuffleGpuTest(const VulkanCtx& ctx,
                            int inC, int inH, int inW, int r,
                            float tolerance)
{
    if (inC % (r * r) != 0) return false;
    const int outC = inC / (r * r);
    const int outH = inH * r;
    const int outW = inW * r;
    const size_t inCount  = (size_t)inC  * inH * inW;
    const size_t outCount = (size_t)outC * outH * outW;
    auto in = deterministicInput(inCount, 0xD2D2D2D2u);
    std::vector<float> cpuOut(outCount);
    referencePixelShuffle(in.data(), cpuOut.data(), inC, inH, inW, r);

    std::vector<float> gpuOut(outCount, 0.0f);
    struct PC { int32_t inH, inW, outC, r; } pc{};
    pc.inH = inH; pc.inW = inW; pc.outC = outC; pc.r = r;

    RunComputeOptions opts{};
    opts.shaderGlsl   = getPixelShuffleShaderGlsl();
    opts.bindingCount = 2;
    opts.buffers[0]   = { in.data(), inCount,  nullptr };
    opts.buffers[1]   = { nullptr,    outCount, gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((outW + 7) / 8);
    opts.dispY        = (uint32_t)((outH + 7) / 8);
    opts.dispZ        = (uint32_t)outC;
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), outCount, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "PixelShuffle r=%d in=%dx%dx%d → %dx%dx%d",
                  r, inC, inH, inW, outC, outH, outW);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runInterpBilinearGpuTest(const VulkanCtx& ctx,
                              int channels, int inH, int inW,
                              int outH, int outW,
                              float tolerance)
{
    const size_t inCount  = (size_t)channels * inH  * inW;
    const size_t outCount = (size_t)channels * outH * outW;
    auto in = deterministicInput(inCount, 0xE3E3E3E3u);
    std::vector<float> cpuOut(outCount);
    referenceInterpBilinear(in.data(), cpuOut.data(),
                            channels, inH, inW, outH, outW);

    std::vector<float> gpuOut(outCount, 0.0f);
    struct PC {
        int32_t inH, inW, outH, outW, channels;
        float   scaleH, scaleW;
        uint32_t _pad;
    } pc{};
    pc.inH = inH; pc.inW = inW; pc.outH = outH; pc.outW = outW;
    pc.channels = channels;
    pc.scaleH = (float)inH / (float)outH;
    pc.scaleW = (float)inW / (float)outW;

    RunComputeOptions opts{};
    opts.shaderGlsl   = getInterpBilinearShaderGlsl();
    opts.bindingCount = 2;
    opts.buffers[0]   = { in.data(), inCount,  nullptr };
    opts.buffers[1]   = { nullptr,    outCount, gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((outW + 7) / 8);
    opts.dispY        = (uint32_t)((outH + 7) / 8);
    opts.dispZ        = (uint32_t)channels;
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), outCount, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "InterpBilinear c=%d %dx%d → %dx%d",
                  channels, inH, inW, outH, outW);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runRifeWarpGpuTest(const VulkanCtx& ctx,
                        int channels, int H, int W,
                        float tolerance)
{
    const size_t hw = (size_t)H * W;
    const size_t imgCount  = (size_t)channels * hw;
    const size_t flowCount = (size_t)2 * hw;
    auto image = deterministicInput(imgCount,  0xF1F1F1F1u);
    // Flow magnitude ~[-1.5, +1.5] so we exercise both +/- direction
    // and edge-clamp.  RIFE optical flow is in pixel units.
    auto flow  = deterministicInput(flowCount, 0xF2F2F2F2u);
    for (auto& v : flow) v *= 3.0f;

    std::vector<float> cpuOut(imgCount);
    referenceRifeWarp(image.data(), flow.data(), cpuOut.data(),
                      channels, H, W);

    std::vector<float> gpuOut(imgCount, 0.0f);
    struct PC { int32_t w, h, c; uint32_t _pad; } pc{};
    pc.w = W; pc.h = H; pc.c = channels;

    RunComputeOptions opts{};
    opts.shaderGlsl   = getRifeWarpShaderGlsl();
    opts.bindingCount = 3;
    opts.buffers[0]   = { image.data(), imgCount,  nullptr };
    opts.buffers[1]   = { flow.data(),  flowCount, nullptr };
    opts.buffers[2]   = { nullptr,       imgCount,  gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((W + 7) / 8);
    opts.dispY        = (uint32_t)((H + 7) / 8);
    opts.dispZ        = (uint32_t)channels;
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), imgCount, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "RifeWarp c=%d %dx%d", channels, H, W);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runDeconv2DGpuTest(const VulkanCtx& ctx,
                        const Model& m,
                        const QString& layerName,
                        float tolerance)
{
    const Layer* L = findLayerByName(m, layerName);
    if (!L || L->kind != OpKind::Deconvolution) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] DeconvTest: layer '%s' not found / not Deconvolution",
                     qUtf8Printable(layerName));
        return false;
    }
    const int N       = paramInt(*L, 0, 0);
    const int kW      = paramInt(*L, 1, 0);
    const int kH      = paramInt(*L, 11, kW);
    const int strideW = paramInt(*L, 3, 1);
    const int strideH = paramInt(*L, 13, strideW);
    const int pad     = paramInt(*L, 4, 0);
    const int hasBias = paramInt(*L, 5, 0);
    const int wsize   = paramInt(*L, 6, 0);
    if (N <= 0 || kW <= 0 || kH <= 0 || wsize <= 0) return false;
    const int C = wsize / (N * kH * kW);  // input channels (ic-outer)
    auto weight = getTensorAsFp32(m, layerName + "/weight");
    auto bias   = hasBias ? getTensorAsFp32(m, layerName + "/bias")
                          : std::vector<float>{};
    if ((int)weight.size() != wsize) return false;

    // Use a 16×16 input — outputs 32×32, modest memory.
    constexpr int W = 16, H = 16;
    const int OUT_H = (H - 1) * strideH + kH - 2 * pad;
    const int OUT_W = (W - 1) * strideW + kW - 2 * pad;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] DeconvTest %s: in=%dx%dx%d → out=%dx%dx%d "
                "(k=%dx%d s=%dx%d p=%d bias=%d)",
                qUtf8Printable(layerName), W, H, C, OUT_W, OUT_H, N,
                kW, kH, strideW, strideH, pad, hasBias);

    auto input = deterministicInput((size_t)C * H * W, 0xDECDEC42u);
    std::vector<float> cpuOut((size_t)N * OUT_H * OUT_W);
    referenceDeconv2D(input.data(), W, H, C,
                      weight.data(), N, kH, kW,
                      hasBias ? bias.data() : nullptr,
                      strideH, strideW, pad, pad,
                      cpuOut.data(), OUT_W, OUT_H);

    std::vector<float> gpuOut(cpuOut.size(), 0.0f);
    struct PC {
        int32_t inDims[4];
        int32_t outDims[4];
        int32_t conv[4];
    } pc{};
    pc.inDims[0]  = W; pc.inDims[1]  = H; pc.inDims[2]  = C; pc.inDims[3]  = hasBias;
    pc.outDims[0] = OUT_W; pc.outDims[1] = OUT_H; pc.outDims[2] = N;
    pc.conv[0]    = kW; pc.conv[1]    = kH; pc.conv[2]    = strideW; pc.conv[3] = pad;

    // Bias buffer must be non-empty even when hasBias=0 (binding 2).
    std::vector<float> biasFallback(N, 0.0f);
    const float* biasPtr = hasBias ? bias.data() : biasFallback.data();

    RunComputeOptions opts{};
    opts.shaderGlsl   = getDeconv2DShaderGlsl();
    opts.bindingCount = 4;
    opts.buffers[0]   = { input.data(),   (size_t)C * H * W,                nullptr };
    opts.buffers[1]   = { weight.data(),  weight.size(),                    nullptr };
    opts.buffers[2]   = { biasPtr,        (size_t)N,                         nullptr };
    opts.buffers[3]   = { nullptr,        (size_t)N * OUT_H * OUT_W,         gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((OUT_W + 7) / 8);
    opts.dispY        = (uint32_t)((OUT_H + 7) / 8);
    opts.dispZ        = (uint32_t)N;
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), cpuOut.size(), &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "DeconvTest %s n=%d ic=%d", qUtf8Printable(layerName), N, C);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

bool runEltwiseSumGpuTest(const VulkanCtx& ctx,
                          size_t count, float c0, float c1,
                          float tolerance)
{
    auto a = deterministicInput(count, 0xFEEDF00Du);
    auto b = deterministicInput(count, 0xDEADBEEFu);
    std::vector<float> cpuOut(count);
    referenceEltwiseSum(a.data(), b.data(), cpuOut.data(), count, c0, c1);

    std::vector<float> gpuOut(count, 0.0f);
    struct PC { uint32_t count; float c0, c1; uint32_t _pad; } pc{};
    pc.count = (uint32_t)count; pc.c0 = c0; pc.c1 = c1;

    RunComputeOptions opts{};
    opts.shaderGlsl   = getEltwiseShaderGlsl();
    opts.bindingCount = 3;
    opts.buffers[0]   = { a.data(), count, nullptr };
    opts.buffers[1]   = { b.data(), count, nullptr };
    opts.buffers[2]   = { nullptr,   count, gpuOut.data() };
    opts.pcData       = &pc;
    opts.pcSize       = sizeof(pc);
    opts.dispX        = (uint32_t)((count + 63) / 64);
    if (!runComputeOnce(ctx, opts)) return false;

    int worst = 0;
    float err = compareF32(cpuOut.data(), gpuOut.data(), count, &worst);
    bool pass = err <= tolerance;
    char label[96];
    std::snprintf(label, sizeof(label),
                  "EltwiseSum count=%zu c0=%.2f c1=%.2f", count, (double)c0, (double)c1);
    logOpResult(label, err, worst, cpuOut[worst], gpuOut[worst], tolerance, pass);
    return pass;
}

// ============================================================================
// Phase 3b.2 standalone — replicates ncnnfruc::runExternalApiProbe's Vulkan
// init pattern, hands handles to ncnn, runs runConv2DGpuTest, tears down.
// Side-effect: claims+releases the ncnn process-singleton (same caveat as
// runExternalApiProbe — env var is dev-only, must abort streaming after
// invoking).
// ============================================================================

bool runConv2DGpuTestStandalone(const QString& modelDir, float tolerance) {
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] standalone GpuTest: starting (modelDir='%s')",
                qUtf8Printable(modelDir));

#ifdef _WIN32
    HMODULE vkLib = ::GetModuleHandleW(L"vulkan-1.dll");
    if (!vkLib) vkLib = ::LoadLibraryW(L"vulkan-1.dll");
    if (!vkLib) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: vulkan-1.dll not loadable");
        return false;
    }
    auto pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        ::GetProcAddress(vkLib, "vkGetInstanceProcAddr"));
#else
    void* vkLib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!vkLib) vkLib = dlopen("libvulkan.so", RTLD_NOW);
    if (!vkLib) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: libvulkan not loadable");
        return false;
    }
    auto pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(vkLib, "vkGetInstanceProcAddr"));
#endif
    if (!pfnGetInstanceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: vkGetInstanceProcAddr missing");
        return false;
    }
    auto pfnCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        pfnGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!pfnCreateInstance) return false;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkPhysicalDevice vkPhys = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue vkQueue = VK_NULL_HANDLE;
    uint32_t computeQF = UINT32_MAX;

    PFN_vkDestroyInstance pfnDestroyInstance = nullptr;
    PFN_vkDestroyDevice   pfnDestroyDevice   = nullptr;

    auto teardown = [&]() {
        if (vkDevice && pfnDestroyDevice) {
            pfnDestroyDevice(vkDevice, nullptr);
            vkDevice = VK_NULL_HANDLE;
        }
        if (vkInstance && pfnDestroyInstance) {
            pfnDestroyInstance(vkInstance, nullptr);
            vkInstance = VK_NULL_HANDLE;
        }
    };

    // ---- VkInstance ----
    {
        VkApplicationInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        ai.pApplicationName = "VipleStream-RifeNativeVkSelfTest";
        ai.apiVersion = VK_API_VERSION_1_2;
        const char* exts[] = { "VK_KHR_get_physical_device_properties2" };
        VkInstanceCreateInfo ici = {};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &ai;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = exts;
        if (pfnCreateInstance(&ici, nullptr, &vkInstance) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] standalone: vkCreateInstance failed");
            return false;
        }
    }
    pfnDestroyInstance = (PFN_vkDestroyInstance)pfnGetInstanceProcAddr(vkInstance, "vkDestroyInstance");

    // ---- Physical device + queue family ----
    auto pfnEnumPhys = (PFN_vkEnumeratePhysicalDevices)pfnGetInstanceProcAddr(vkInstance, "vkEnumeratePhysicalDevices");
    auto pfnGetPhysProps = (PFN_vkGetPhysicalDeviceProperties)pfnGetInstanceProcAddr(vkInstance, "vkGetPhysicalDeviceProperties");
    auto pfnGetQFProps = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)pfnGetInstanceProcAddr(vkInstance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnCreateDevice = (PFN_vkCreateDevice)pfnGetInstanceProcAddr(vkInstance, "vkCreateDevice");
    if (!pfnEnumPhys || !pfnGetPhysProps || !pfnGetQFProps || !pfnCreateDevice) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: missing instance entries");
        teardown();
        return false;
    }
    uint32_t physCount = 0;
    pfnEnumPhys(vkInstance, &physCount, nullptr);
    if (physCount == 0) { teardown(); return false; }
    std::vector<VkPhysicalDevice> phys(physCount);
    pfnEnumPhys(vkInstance, &physCount, phys.data());
    vkPhys = phys[0];
    VkPhysicalDeviceProperties physProps;
    pfnGetPhysProps(vkPhys, &physProps);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] standalone: picked '%s' (api=%u.%u)",
                physProps.deviceName,
                VK_VERSION_MAJOR(physProps.apiVersion),
                VK_VERSION_MINOR(physProps.apiVersion));
    uint32_t qfCount = 0;
    pfnGetQFProps(vkPhys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qfCount);
    pfnGetQFProps(vkPhys, &qfCount, qfp.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { computeQF = i; break; }
    }
    if (computeQF == UINT32_MAX) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: no compute queue family");
        teardown();
        return false;
    }

    // ---- VkDevice ----
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = computeQF;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci = {};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        if (pfnCreateDevice(vkPhys, &dci, nullptr, &vkDevice) != VK_SUCCESS) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] standalone: vkCreateDevice failed");
            teardown();
            return false;
        }
    }
    pfnDestroyDevice = (PFN_vkDestroyDevice)pfnGetInstanceProcAddr(vkInstance, "vkDestroyDevice");

    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)pfnGetInstanceProcAddr(vkInstance, "vkGetDeviceProcAddr");
    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)pfnGetDeviceProcAddr(vkDevice, "vkGetDeviceQueue");
    pfnGetDeviceQueue(vkDevice, computeQF, 0, &vkQueue);

    // ---- Hand to ncnn so compile_spirv_module works ----
    int rc = ncnn::create_gpu_instance_external(
        vkInstance, vkPhys, vkDevice,
        computeQF, 1,
        computeQF, 1,
        computeQF, 1);
    if (rc != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: create_gpu_instance_external rc=%d", rc);
        teardown();
        return false;
    }

    // ---- Load model once + run correctness gates against the trusted
    //      CPU reference for representative Conv layers covering both
    //      stride values, the ic=3 RGB entry path, the dominant
    //      stride=1 path (44/56 conv layers), and the largest channel
    //      count present in the model (n=192).  Phase 4a coverage. ----
    Model m;
    if (!parseParam(modelDir + "/flownet.param", m)
        || !loadWeights(modelDir + "/flownet.bin", m)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-RIFE-VK] standalone: model load failed");
        ncnn::destroy_gpu_instance();
        teardown();
        return false;
    }
    VulkanCtx ctx{};
    ctx.instance            = vkInstance;
    ctx.physicalDevice      = vkPhys;
    ctx.device              = vkDevice;
    ctx.computeQueueFamily  = computeQF;
    ctx.computeQueue        = vkQueue;
    ctx.getInstanceProcAddr = (void*)pfnGetInstanceProcAddr;
    bool pass = true;
    pass &= runConv2DGpuTest(ctx, m, "Conv_16", tolerance);  // s=2 ic=3  n=16  LeakyReLU
    pass &= runConv2DGpuTest(ctx, m, "Conv_18", tolerance);  // s=1 ic=16 n=16  LeakyReLU
    pass &= runConv2DGpuTest(ctx, m, "Conv_50", tolerance);  // s=2 ic=96 n=192 LeakyReLU

    // ---- Phase 4b BinaryOp coverage: 3 broadcast modes × representative ops ----
    {
        // Synthesize NCHW = (1, 16, 8, 8) deterministic input.
        constexpr int C = 16, HW = 64;       // hw = 8*8
        constexpr int N = C * HW;            // 1024 elements
        std::vector<float> a(N), b_same(N), b_chan(C);
        uint32_t st = 0x42424242u;
        auto next = [&]() {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            return (float)(st & 0xFFFFFF) / (float)0xFFFFFF - 0.5f;
        };
        for (auto& v : a)      v = next();
        for (auto& v : b_same) v = next();
        for (auto& v : b_chan) v = next() + 0.6f;  // keep > 0 so DIV won't blow up

        // 1. same-shape ADD (residual skip)
        BinaryOpDesc d1{}; d1.opType = 0; d1.mode = 0;
        pass &= runBinaryOpGpuTest(ctx, d1, a, b_same, tolerance);
        // 2. channel-broadcast MUL × beta
        BinaryOpDesc d2{}; d2.opType = 2; d2.mode = 1;
        d2.channels = C; d2.hw = HW;
        pass &= runBinaryOpGpuTest(ctx, d2, a, b_chan, tolerance);
        // 3. scalar DIV (Div_146 pattern: x / 16.0)
        BinaryOpDesc d3{}; d3.opType = 3; d3.mode = 2; d3.scalarB = 16.0f;
        pass &= runBinaryOpGpuTest(ctx, d3, a, {}, tolerance);
        // 4. scalar RSUB (Sub_519 pattern: 1.0 - x)
        BinaryOpDesc d4{}; d4.opType = 7; d4.mode = 2; d4.scalarB = 1.0f;
        pass &= runBinaryOpGpuTest(ctx, d4, a, {}, tolerance);
    }

    // ---- Phase 4c Activation coverage: LeakyReLU 0.2 (40 layers) + Sigmoid (1) ----
    {
        constexpr int N = 1024;
        std::vector<float> a(N);
        uint32_t st = 0xCAFEBABEu;
        // Centered range [-3, +3] so we exercise both branches of ReLU
        // and a meaningful slice of Sigmoid's curve.
        for (auto& v : a) {
            st ^= st << 13; st ^= st >> 17; st ^= st << 5;
            v = ((float)(st & 0xFFFFFF) / (float)0xFFFFFF - 0.5f) * 6.0f;
        }
        // 1. Pure ReLU (slope=0)
        ActivationDesc r1{}; r1.actType = 0; r1.slope = 0.0f;
        pass &= runActivationGpuTest(ctx, r1, a, tolerance);
        // 2. LeakyReLU 0.2 (the 40-layer case)
        ActivationDesc r2{}; r2.actType = 0; r2.slope = 0.2f;
        pass &= runActivationGpuTest(ctx, r2, a, tolerance);
        // 3. Sigmoid — fp32 transcendental, expect slightly looser tolerance
        ActivationDesc r3{}; r3.actType = 1;
        pass &= runActivationGpuTest(ctx, r3, a, 1e-6f);
    }

    // ---- Phase 4d shape-op coverage ----
    // Crop  — slice channels [2:5] from a (8, 4, 4) tensor (covers
    //         the largest distinct param sig in the audit: ic=8, slice
    //         contiguous channel range).
    pass &= runCropGpuTest(ctx, /*inC*/8, /*inH*/4, /*inW*/4,
                                /*cStart*/2, /*cEnd*/5);
    // Concat — concatenate (3,4,4) + (5,4,4) along channels.
    pass &= runConcatGpuTest(ctx, /*inA_C*/3, /*inB_C*/5,
                                  /*H*/4, /*W*/4);
    // PixelShuffle r=2 — (16, 4, 4) → (4, 8, 8).
    pass &= runPixelShuffleGpuTest(ctx, /*inC*/16, /*inH*/4, /*inW*/4, /*r*/2);
    // Interp bilinear — (4, 8, 8) → (4, 16, 16) and (4, 16, 16) → (4, 4, 4).
    pass &= runInterpBilinearGpuTest(ctx, /*c*/4, /*inH*/8,  /*inW*/8,
                                          /*outH*/16, /*outW*/16);
    pass &= runInterpBilinearGpuTest(ctx, /*c*/4, /*inH*/16, /*inW*/16,
                                          /*outH*/4,  /*outW*/4);
    // EltwiseSum — covers the 3 RIFE coeff variants [1.0, k] for k ∈ {4,8,16}.
    pass &= runEltwiseSumGpuTest(ctx, /*count*/512, /*c0*/1.0f, /*c1*/4.0f);
    pass &= runEltwiseSumGpuTest(ctx, /*count*/512, /*c0*/1.0f, /*c1*/8.0f);
    pass &= runEltwiseSumGpuTest(ctx, /*count*/512, /*c0*/1.0f, /*c1*/16.0f);

    // ---- Phase 4e Deconvolution coverage: smallest + largest weight ----
    pass &= runDeconv2DGpuTest(ctx, m, "ConvTranspose_22");   // n=4   ic=16
    pass &= runDeconv2DGpuTest(ctx, m, "ConvTranspose_84");   // n=52  ic=192

    // ---- Phase 4f rife.Warp coverage: 1 case (math is shape-agnostic) ----
    pass &= runRifeWarpGpuTest(ctx, /*channels*/8, /*H*/16, /*W*/16);

    // ---- Phase 4g.2 pipeline cache smoke ----
    pass &= runPipelineCacheSmoke(ctx);

    // ---- Phase 4g.3 buffer pool smoke ----
    pass &= runBlobBufferPoolSmoke(ctx, modelDir);

    // ---- Phase 4g.1 shape inference smoke ----
    {
        InferInputs in{};
        in.in0 = { 3, 256, 256 };   // prev RGB at 256×256
        in.in1 = { 3, 256, 256 };   // curr RGB at 256×256
        in.in2 = { 1,   1,   1 };   // timestep scalar
        std::unordered_map<QString, BlobShape> blobShapes;
        if (!inferBlobShapes(m, in, blobShapes)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-RIFE-VK] Phase 4g.1: inferBlobShapes FAILED");
            pass = false;
        } else {
            // Sanity-check shapes we know from the audit:
            //   Conv_16 output: (16, 128, 128) — 1st conv stride=2
            //   Conv_18 output: (16, 128, 128) — stride=1
            //   Conv_50 output: (192, 32, 32)  — bigger channel
            //   ConvTranspose_22 output: (4, 256, 256) — 2× upsample
            auto check = [&](const char* layerName,
                             const char* outName,
                             BlobShape want) {
                auto it = blobShapes.find(outName);
                if (it == blobShapes.end()) {
                    std::fprintf(stderr,
                        "[VIPLE-RIFE-VK] 4g.1: blob '%s' for layer '%s' not found ✗\n",
                        outName, layerName);
                    pass = false;
                    return;
                }
                if (!(it->second == want)) {
                    std::fprintf(stderr,
                        "[VIPLE-RIFE-VK] 4g.1 anchor %s '%s' = (%d,%d,%d), want (%d,%d,%d) ✗\n",
                        layerName, outName,
                        it->second.c, it->second.h, it->second.w,
                        want.c, want.h, want.w);
                    pass = false;
                } else {
                    std::fprintf(stderr,
                        "[VIPLE-RIFE-VK] 4g.1 anchor %s '%s' = (%d,%d,%d) ✓\n",
                        layerName, outName,
                        it->second.c, it->second.h, it->second.w);
                }
            };
            // Look up the actual output blob name for each anchor layer:
            auto resolveOutput = [&](const char* layerName) -> QString {
                for (const auto& L : m.layers) {
                    if (L.name == layerName && !L.outputs.isEmpty()) return L.outputs[0];
                }
                return {};
            };
            QString c16Out  = resolveOutput("Conv_16");
            QString c18Out  = resolveOutput("Conv_18");
            QString ct22Out = resolveOutput("ConvTranspose_22");
            // Hand-checked anchors (RIFE pipeline is too branchy to
            // mentally trace beyond the first 2 conv + the matching
            // upsample; 485-count match below is the real correctness
            // signal):
            //   Conv_16 (s=2):           (3, 256, 256) → (16, 128, 128)
            //   Conv_18 (s=1):           (16, 128, 128) → (16, 128, 128)
            //   ConvTranspose_22 (s=2):  (16, 128, 128) → (4, 256, 256)
            check("Conv_16",          qUtf8Printable(c16Out),  { 16, 128, 128 });
            check("Conv_18",          qUtf8Printable(c18Out),  { 16, 128, 128 });
            check("ConvTranspose_22", qUtf8Printable(ct22Out), {  4, 256, 256 });
            std::fprintf(stderr,
                "[VIPLE-RIFE-VK] 4g.1: %zu blob shapes inferred (model claims %d) — %s\n",
                blobShapes.size(), m.blobCount, pass ? "PASS" : "FAIL");
            std::fflush(stderr);
        }
    }

    // ---- Cleanup ----
    ncnn::destroy_gpu_instance();
    teardown();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] standalone GpuTest: %s",
                pass ? "PASS" : "FAIL");
    return pass;
}

} // namespace viple::rife_native_vk

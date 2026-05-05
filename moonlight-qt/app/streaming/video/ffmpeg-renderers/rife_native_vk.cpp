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

} // anonymous namespace

// Helper: lookup a layer by exact name in the parsed model.  Returns
// nullptr if the layer isn't present.
static const Layer* findLayerByName(const Model& m, const QString& name) {
    for (const auto& L : m.layers) {
        if (L.name == name) return &L;
    }
    return nullptr;
}

// Helper: read an int param with default if absent.
static int paramInt(const Layer& L, int id, int def) {
    auto it = L.params.find(id);
    if (it == L.params.end()) return def;
    return (int)it->second.i;
}

// Helper: read the LeakyReLU slope from activation_params (-23310 array)
// when activation_type == 2.  Returns 0.0f when no LeakyReLU.
static float leakyReluSlopeOf(const Layer& L) {
    int actType = paramInt(L, 9, 0);
    if (actType != 2) return 0.0f;
    auto it = L.params.find(-23310);
    if (it == L.params.end()) return 0.0f;
    if (it->second.fa.empty()) return 0.0f;
    return (float)it->second.fa[0];
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

    // ---- Cleanup ----
    ncnn::destroy_gpu_instance();
    teardown();

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-RIFE-VK] standalone GpuTest: %s",
                pass ? "PASS" : "FAIL");
    return pass;
}

} // namespace viple::rife_native_vk

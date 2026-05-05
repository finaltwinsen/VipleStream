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
}

} // namespace viple::rife_native_vk

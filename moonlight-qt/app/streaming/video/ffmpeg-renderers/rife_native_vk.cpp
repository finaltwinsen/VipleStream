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
}

} // namespace viple::rife_native_vk

// VipleStream §SF-PROBE（M2a）— `viplestream decode-bench` 實作；介面、輸入格式與結束碼見
// decodebench.h。
//
// 為什麼自己寫解碼迴圈：FFmpegVideoDecoder 的非測試模式要 Session（overlay、Pacer），
// fruc-offline 又根本不解碼（scout E §2.1）。這裡只用 libavcodec（app 不連 libavformat），
// AVCodecContext 的設定逐項照 ffmpeg.cpp completeInitialization()，量到的才是串流時
// decoder 的行為。
//
// 流程：
//   1. 讀檔、切 AU：H.264／HEVC 用 av_parser_parse2 切 Annex-B；AV1 自己解 IVF。整個檔案
//      先切好放記憶體，切割成本不進計時。
//   2. 候選 decoder：auto = <codec>_v4l2m2m → hwaccel（avcodec_get_hw_config）→ SW。auto
//      模式下每個候選先試解前幾個 AU，能出幀才用（等同 app 啟動時的測試幀），再重開一個
//      乾淨的 instance 給計時用。
//   3. --compare-sw：先用 SW decoder 把整個檔案解一遍當參考（不計時），存取樣後的 luma。
//   4. 計時迴圈：pts = 全域幀號 g；--fps 時依 g/fps 排程送出（模擬串流），否則連發量吞吐。
//      送出後照 app decoder 執行緒的模型輪詢 avcodec_receive_frame（沒輸出就小睡再試）。
//   5. 統計、事件（掉幀／破損／錯誤）的癒合分析、印摘要、寫 JSON。

#include "decodebench.h"
#include "probeutil.h"
#include "backend/sfenv.h"

#include <QtGlobal>
#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <SDL.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#ifdef HAVE_FFMPEG
// PACER_MAX_OUTSTANDING_FRAMES（v4l2m2m capture buffer 數要和 app 同一個公式）與
// MAX_SLICES（經 decoder.h）。這條 include 鏈不會帶進 common-c 的 PlatformSockets.h。
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

// DRM_PRIME 的 descriptor 只需要 libavutil 的標頭（不需要 libdrm）；x64 AppImage 帶
// disable-libdrm 也照樣能描述。CPU 取樣（--compare-sw）另外要 mmap。
#if defined(Q_OS_LINUX) && __has_include(<libavutil/hwcontext_drm.h>)
#define DECODEBENCH_HAVE_DRM 1
extern "C" {
#include <libavutil/hwcontext_drm.h>
}
#include <sys/mman.h>
#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#define DECODEBENCH_HAVE_DMABUF_SYNC 1
#endif
#endif

#ifdef HAVE_LIBPLACEBO_VULKAN
#include <libplacebo/log.h>
#include <libplacebo/renderer.h>   // struct pl_frame 的完整定義（同 plvk.h）
#include <libplacebo/vulkan.h>
// 實作在 plvk_c.c（PL_LIBAV_IMPLEMENTATION 1），這裡只要宣告
#define PL_LIBAV_IMPLEMENTATION 0
#include <libplacebo/utils/libav.h>
extern "C" {
#include <libavutil/hwcontext_vulkan.h>
}
#endif

#if defined(_WIN32) && defined(WSAEWOULDBLOCK)
// 防回歸：這個檔案的 AVERROR(EAGAIN) 必須是 CRT 的 errno，不能是 common-c
// PlatformSockets.h 的 WSA 版（見 ffmpeg.cpp 檔頭 VIPLE_MPQUIC include 的 push/pop_macro）。
// 這裡本來就不 include 任何 common-c socket 標頭。
static_assert(EAGAIN != WSAEWOULDBLOCK, "EAGAIN must keep its CRT value in decodebench.cpp");
#endif
#endif // HAVE_FFMPEG

namespace {

const char k_Tag[] = "VIPLE-V4L2";

QJsonObject optionsJson(const DecodeBenchOptions& o)
{
    QJsonObject j;
    j[QStringLiteral("file")] = o.file;
    j[QStringLiteral("codec")] = o.codec;
    j[QStringLiteral("decoder")] = o.decoder;
    j[QStringLiteral("out")] = o.out;
    j[QStringLiteral("fps")] = o.fps;
    j[QStringLiteral("frames")] = o.frames;
    j[QStringLiteral("loop")] = o.loop;
    j[QStringLiteral("warmup")] = o.warmup;
    j[QStringLiteral("captureBuffers")] = o.captureBuffers;
    j[QStringLiteral("outputBuffers")] = o.outputBuffers;
    j[QStringLiteral("hold")] = o.hold;
    QJsonArray drop, corrupt;
    for (int k : o.dropFrames) {
        drop.append(k);
    }
    for (int k : o.corruptFrames) {
        corrupt.append(k);
    }
    j[QStringLiteral("dropFrames")] = drop;
    j[QStringLiteral("dropEvery")] = o.dropEvery;
    j[QStringLiteral("corruptFrames")] = corrupt;
    j[QStringLiteral("flushOnError")] = o.flushOnError;
    j[QStringLiteral("compareSw")] = o.compareSw;
    j[QStringLiteral("mapVulkan")] = o.mapVulkan;
    j[QStringLiteral("verbose")] = o.verbose;
    return j;
}

} // namespace

#ifndef HAVE_FFMPEG

int runDecodeBench(const DecodeBenchOptions& options)
{
    ProbeUtil::beginProbe("decode-bench");
    const QString jsonPath = options.jsonPath.isEmpty()
            ? ProbeUtil::defaultJsonPath(QStringLiteral("decode-bench"))
            : options.jsonPath;

    ProbeUtil::printLine(k_Tag, "bench: unavailable (this build has no FFmpeg)");

    QJsonObject root;
    root[QStringLiteral("probe")] = QStringLiteral("decode-bench");   // 根鍵與 xr-probe／v4l2-probe 一致
    root[QStringLiteral("schema")] = 1;
    root[QStringLiteral("options")] = optionsJson(options);
    root[QStringLiteral("error")] = QStringLiteral("built without FFmpeg");
    root[QStringLiteral("rc")] = (int)ProbeUtil::kExitNotBuilt;
    root[QStringLiteral("env")] = SfEnv::snapshot();
    const bool written = ProbeUtil::writeJson(jsonPath, root);
    return ProbeUtil::finish(ProbeUtil::kExitNotBuilt, written ? jsonPath : QString());
}

#else // HAVE_FFMPEG

namespace {

using Clock = std::chrono::steady_clock;

// 輪詢 receive_frame 的間隔。app 的 decoder 執行緒在「沒輸入也沒輸出」時睡 1 ms；這裡取更細，
// 免得量化誤差蓋掉 sub-ms 的差異。Windows 的 sleep 精度靠 SDL timer 子系統把系統計時器
// 調到 1 ms（runDecodeBench 開頭初始化）。
constexpr int k_PollUs = 250;
// avcodec_send_packet 連續 EAGAIN（輸入滿、又收不到幀）超過這個時間 → 判定卡死並中止。
// PoC-3c 的 --hold 大於 capture buffer 可用數時會走到這裡，這本身就是答案。
constexpr double k_DeadlockMs = 5000.0;
// 「完全癒合」門檻（PoC-3b／G-rc）
constexpr double k_HealPsnrDb = 40.0;
constexpr int k_MaxEventLines = 20;
constexpr int k_MaxJsonEvents = 2000;
// 一次計時的總幀數（AU 數 × --loop）上限。per-frame 表（m_Status 等 5 個 vector）每幀約 22 B，
// 延遲與送出阻塞統計再各 8 B，1e7 幀約 0.4 GB；同時保證 m_Total、g 這些 int 不會溢位
constexpr qint64 k_MaxScheduledFrames = 10000000;
static_assert(k_MaxScheduledFrames <= INT_MAX, "m_Total is an int");
// --compare-sw 參考 luma 的記憶體預算（每幀取樣數由它和幀數推出，4096..65536）
constexpr qint64 k_RefBudgetBytes = 512LL * 1024 * 1024;

// auto 的 hwaccel 嘗試順序：Windows 串流預設 D3D11VA；Linux 串流預設 VAAPI（§K.4），其次
// Vulkan。名稱經 av_hwdevice_find_type_by_name 轉換，這版 FFmpeg 沒有的型別自動略過。
const char* const k_HwPreference[] = {
    "d3d11va", "vaapi", "vulkan", "vdpau", "cuda", "dxva2", "d3d12va", "videotoolbox", "drm",
};

double msBetween(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

void sleepPoll()
{
    std::this_thread::sleep_for(std::chrono::microseconds(k_PollUs));
}

double round3(double v)
{
    return std::round(v * 1000.0) / 1000.0;
}

QString avErr(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return QStringLiteral("%1 (%2)").arg(QString::fromUtf8(buf)).arg(err);
}

const char* pixFmtName(int fmt)
{
    const char* name = av_get_pix_fmt_name((AVPixelFormat)fmt);
    return name ? name : "none";
}

QString fourccString(uint32_t f)
{
    QString s;
    for (int i = 0; i < 4; i++) {
        const char c = (char)((f >> (8 * i)) & 0xFF);
        s += (c >= 0x20 && c < 0x7F) ? QLatin1Char(c) : QLatin1Char('?');
    }
    return s;
}

uint16_t rl16(const uint8_t* p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

uint32_t rl32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------------------------------------------------------------------------
// 統計

struct Dist {
    int n = 0;
    double mean = 0, p50 = 0, p95 = 0, p99 = 0, maxv = 0;
};

Dist makeDist(std::vector<double> v)
{
    Dist d;
    d.n = (int)v.size();
    if (v.empty()) {
        return d;
    }
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double x : v) {
        sum += x;
    }
    d.mean = sum / (double)v.size();
    // nearest-rank 百分位
    auto pct = [&v](double q) {
        size_t idx = (size_t)std::ceil(q * (double)v.size());
        if (idx < 1) {
            idx = 1;
        }
        if (idx > v.size()) {
            idx = v.size();
        }
        return v[idx - 1];
    };
    d.p50 = pct(0.50);
    d.p95 = pct(0.95);
    d.p99 = pct(0.99);
    d.maxv = v.back();
    return d;
}

QJsonObject distJson(const Dist& d)
{
    QJsonObject j;
    j[QStringLiteral("n")] = d.n;
    j[QStringLiteral("mean")] = round3(d.mean);
    j[QStringLiteral("p50")] = round3(d.p50);
    j[QStringLiteral("p95")] = round3(d.p95);
    j[QStringLiteral("p99")] = round3(d.p99);
    j[QStringLiteral("max")] = round3(d.maxv);
    return j;
}

// ---------------------------------------------------------------------------
// 輸入

struct BenchPacket {
    QByteArray data;   // 尾端多 AV_INPUT_BUFFER_PADDING_SIZE 個 0（FFmpeg 對輸入的要求）
    int size = 0;
    bool key = false;
};

struct InputInfo {
    QString path;
    QString codec;
    QString container;
    AVCodecID codecId = AV_CODEC_ID_NONE;
    int width = 0;
    int height = 0;
    qint64 fileBytes = 0;
    int keyframes = 0;
    bool truncatedTail = false;
    int ivfDeclaredFrames = -1;
    int ivfEmptyFrames = 0;
};

BenchPacket makePacket(const uint8_t* p, int n, bool key)
{
    BenchPacket pkt;
    pkt.data = QByteArray(n + AV_INPUT_BUFFER_PADDING_SIZE, '\0');
    memcpy(pkt.data.data(), p, (size_t)n);
    pkt.size = n;
    pkt.key = key;
    return pkt;
}

// AV1：temporal unit 內有 sequence header OBU 就當成 key（Sunshine 的 AV1 IDR 會帶）
bool av1HasSequenceHeader(const uint8_t* p, size_t n)
{
    size_t pos = 0;
    while (pos < n) {
        const uint8_t h = p[pos];
        const int type = (h >> 3) & 0x0F;
        const bool hasExt = (h & 0x04) != 0;
        const bool hasSize = (h & 0x02) != 0;
        size_t hdr = 1 + (hasExt ? 1 : 0);
        if (pos + hdr > n) {
            break;
        }
        uint64_t obuSize = 0;
        if (hasSize) {
            size_t i = pos + hdr;
            bool done = false;
            for (int b = 0, shift = 0; b < 8 && i < n; b++, shift += 7) {
                const uint8_t byte = p[i++];
                obuSize |= (uint64_t)(byte & 0x7F) << shift;
                if (!(byte & 0x80)) {
                    done = true;
                    break;
                }
            }
            if (!done) {
                break;
            }
            hdr = i - pos;
        }
        else {
            obuSize = n - pos - hdr;
        }
        if (type == 1) { // OBU_SEQUENCE_HEADER
            return true;
        }
        if (obuSize > n - pos - hdr) {
            break;
        }
        pos += hdr + (size_t)obuSize;
    }
    return false;
}

bool splitAnnexB(QByteArray& raw, InputInfo& info, QVector<BenchPacket>& pkts, QString& err)
{
    info.container = QStringLiteral("annexb");

    AVCodecParserContext* parser = av_parser_init(info.codecId);
    if (parser == nullptr) {
        err = QStringLiteral("this FFmpeg has no parser for %1").arg(info.codec);
        return false;
    }
    // parser 會改寫 avctx 的欄位（profile、level…），所以給它一個獨立的 context
    AVCodecContext* pctx = avcodec_alloc_context3(nullptr);
    if (pctx == nullptr) {
        av_parser_close(parser);
        err = QStringLiteral("avcodec_alloc_context3 failed");
        return false;
    }
    pctx->codec_type = AVMEDIA_TYPE_VIDEO;
    pctx->codec_id = info.codecId;

    const qint64 total = raw.size();
    // parser 假設輸入尾端還有 AV_INPUT_BUFFER_PADDING_SIZE 可讀
    raw.append(QByteArray(AV_INPUT_BUFFER_PADDING_SIZE, '\0'));
    const uint8_t* base = reinterpret_cast<const uint8_t*>(raw.constData());

    auto takeOutput = [&](const uint8_t* out, int outSize) {
        if (out == nullptr || outSize <= 0) {
            return;
        }
        const bool key = parser->key_frame == 1;
        if (key) {
            info.keyframes++;
        }
        if (info.width == 0 && parser->width > 0 && parser->height > 0) {
            info.width = parser->width;
            info.height = parser->height;
        }
        pkts.append(makePacket(out, outSize, key));
    };

    bool ok = true;
    qint64 pos = 0;
    while (pos < total) {
        const int chunk = (int)qMin<qint64>(total - pos, 1 << 20);
        uint8_t* out = nullptr;
        int outSize = 0;
        const int used = av_parser_parse2(parser, pctx, &out, &outSize, base + pos, chunk,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, pos);
        if (used < 0) {
            err = QStringLiteral("av_parser_parse2 failed at offset %1: %2").arg(pos).arg(avErr(used));
            ok = false;
            break;
        }
        pos += used;
        takeOutput(out, outSize);
        if (used == 0 && outSize == 0) {
            err = QStringLiteral("parser made no progress at offset %1").arg(pos);
            ok = false;
            break;
        }
    }

    // EOF：把 parser 裡最後一個 AU 擠出來
    for (int i = 0; ok && i < 4; i++) {
        uint8_t* out = nullptr;
        int outSize = 0;
        av_parser_parse2(parser, pctx, &out, &outSize, nullptr, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, total);
        if (outSize <= 0) {
            break;
        }
        takeOutput(out, outSize);
    }

    av_parser_close(parser);
    avcodec_free_context(&pctx);

    if (ok && pkts.isEmpty()) {
        err = QStringLiteral("no access units found (is this Annex-B %1?)").arg(info.codec);
        ok = false;
    }
    return ok;
}

// IVF：32 B 檔頭 + 每幀 12 B 幀頭。檔頭的幀數不可信（dump 固定寫 0）；最後一幀被截斷
// （錄製被 Stop-Process -Force 結束）時丟掉那一幀，其餘照用。
bool splitIvf(const QByteArray& raw, InputInfo& info, QVector<BenchPacket>& pkts, QString& err)
{
    info.container = QStringLiteral("ivf");
    const uint8_t* d = reinterpret_cast<const uint8_t*>(raw.constData());
    const qint64 n = raw.size();
    if (n < 32) {
        err = QStringLiteral("IVF header truncated");
        return false;
    }
    const int hdrLen = rl16(d + 6);
    if (hdrLen < 32 || hdrLen > n) {
        err = QStringLiteral("bad IVF header length %1").arg(hdrLen);
        return false;
    }
    const QString fourcc = fourccString(rl32(d + 8));
    if (fourcc != QLatin1String("AV01")) {
        err = QStringLiteral("IVF fourcc is %1, expected AV01").arg(fourcc);
        return false;
    }
    info.width = rl16(d + 12);
    info.height = rl16(d + 14);
    info.ivfDeclaredFrames = (int)rl32(d + 24);

    qint64 pos = hdrLen;
    while (pos < n) {
        if (n - pos < 12) {
            info.truncatedTail = true;
            break;
        }
        const uint32_t frameSize = rl32(d + pos);
        pos += 12;
        if (frameSize == 0) {
            info.ivfEmptyFrames++;
            continue;
        }
        if (frameSize > (256u << 20) || (qint64)frameSize > n - pos) {
            info.truncatedTail = true;
            break;
        }
        const bool key = av1HasSequenceHeader(d + pos, frameSize);
        if (key) {
            info.keyframes++;
        }
        pkts.append(makePacket(d + pos, (int)frameSize, key));
        pos += frameSize;
    }

    if (pkts.isEmpty()) {
        err = QStringLiteral("IVF file has no complete frames");
        return false;
    }
    return true;
}

int loadInput(const DecodeBenchOptions& o, InputInfo& info, QVector<BenchPacket>& pkts, QString& err)
{
    info.path = o.file;
    if (o.file.isEmpty()) {
        err = QStringLiteral("no input file");
        return ProbeUtil::kExitBadInput;
    }

    QFile f(o.file);
    if (!f.open(QIODevice::ReadOnly)) {
        const QFileInfo fi(o.file);
        const int e = !fi.exists() ? ENOENT : (!fi.isReadable() ? EACCES : EIO);
        err = QStringLiteral("cannot open %1: %2").arg(o.file, ProbeUtil::sandboxPathHint(o.file, e));
        return ProbeUtil::kExitBadInput;
    }
    QByteArray raw = f.readAll();
    f.close();
    info.fileBytes = raw.size();
    if (raw.isEmpty()) {
        err = QStringLiteral("input file is empty");
        return ProbeUtil::kExitBadInput;
    }

    const bool isIvf = raw.size() >= 4 && memcmp(raw.constData(), "DKIF", 4) == 0;
    QString codec = o.codec.trimmed().toLower();
    if (codec == QLatin1String("h265")) {
        codec = QStringLiteral("hevc");
    }
    if (codec.isEmpty() || codec == QLatin1String("auto")) {
        const QString ext = QFileInfo(o.file).suffix().toLower();
        if (isIvf) {
            codec = QStringLiteral("av1");
        }
        else if (ext == QLatin1String("h264") || ext == QLatin1String("264") || ext == QLatin1String("avc")) {
            codec = QStringLiteral("h264");
        }
        else if (ext == QLatin1String("h265") || ext == QLatin1String("265") || ext == QLatin1String("hevc")) {
            codec = QStringLiteral("hevc");
        }
        else {
            err = QStringLiteral("cannot infer codec from extension .%1; pass --codec h264|hevc|av1").arg(ext);
            return ProbeUtil::kExitBadInput;
        }
    }

    if (codec == QLatin1String("h264")) {
        info.codecId = AV_CODEC_ID_H264;
    }
    else if (codec == QLatin1String("hevc")) {
        info.codecId = AV_CODEC_ID_HEVC;
    }
    else if (codec == QLatin1String("av1")) {
        info.codecId = AV_CODEC_ID_AV1;
    }
    else {
        err = QStringLiteral("unknown codec %1 (h264|hevc|av1)").arg(codec);
        return ProbeUtil::kExitBadInput;
    }
    info.codec = codec;

    if (info.codecId == AV_CODEC_ID_AV1) {
        if (!isIvf) {
            err = QStringLiteral("AV1 input must be IVF (record it with stream --dump-bitstream)");
            return ProbeUtil::kExitBadInput;
        }
        return splitIvf(raw, info, pkts, err) ? ProbeUtil::kExitOk : ProbeUtil::kExitBadInput;
    }
    if (isIvf) {
        err = QStringLiteral("file is IVF but codec is %1").arg(codec);
        return ProbeUtil::kExitBadInput;
    }
    return splitAnnexB(raw, info, pkts, err) ? ProbeUtil::kExitOk : ProbeUtil::kExitBadInput;
}

// ---------------------------------------------------------------------------
// decoder 候選與開啟

enum class DecKind { Software, V4l2m2m, HwAccel, Hardware };

const char* kindName(DecKind k)
{
    switch (k) {
    case DecKind::Software: return "sw";
    case DecKind::V4l2m2m: return "v4l2m2m";
    case DecKind::HwAccel: return "hwaccel";
    case DecKind::Hardware: return "hw";
    }
    return "?";
}

struct BenchCandidate {
    const AVCodec* codec = nullptr;
    DecKind kind = DecKind::Software;
    const AVCodecHWConfig* hwCfg = nullptr;
    QString label;
};

BenchCandidate makeCandidate(const AVCodec* codec, DecKind kind, const AVCodecHWConfig* hwCfg, const QString& label)
{
    BenchCandidate c;
    c.codec = codec;
    c.kind = kind;
    c.hwCfg = hwCfg;
    c.label = label;
    return c;
}

const char* nativeDecoderName(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return "h264";
    case AV_CODEC_ID_HEVC: return "hevc";
    case AV_CODEC_ID_AV1: return "av1";
    default: return "";
    }
}

// SW decoder 名稱，順序同 app（ffmpeg.cpp 的 VkFruc SW 路徑）。原生 av1 decoder 只做 hwaccel，
// 不能當軟解，所以 AV1 只列 libdav1d（其次 libaom）。
QStringList swDecoderNames(AVCodecID id)
{
    switch (id) {
    case AV_CODEC_ID_H264: return {QStringLiteral("h264")};
    case AV_CODEC_ID_HEVC: return {QStringLiteral("hevc")};
    case AV_CODEC_ID_AV1: return {QStringLiteral("libdav1d"), QStringLiteral("libaom-av1")};
    default: return {};
    }
}

DecKind classifyNamedDecoder(const AVCodec* c)
{
    const QString name = QString::fromUtf8(c->name);
    if (name.endsWith(QLatin1String("_v4l2m2m"), Qt::CaseInsensitive)) {
        return DecKind::V4l2m2m;
    }
    // 同 ffmpeg.cpp getAVCodecCapabilities()：*_omx 忘了設 AV_CODEC_CAP_HARDWARE
    if ((c->capabilities & AV_CODEC_CAP_HARDWARE) || name.endsWith(QLatin1String("_omx"), Qt::CaseInsensitive)) {
        return DecKind::Hardware;
    }
    return DecKind::Software;
}

const AVCodecHWConfig* findHwConfig(const AVCodec* c, AVHWDeviceType type)
{
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
        if (cfg == nullptr) {
            return nullptr;
        }
        if (cfg->device_type == type && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
            return cfg;
        }
    }
}

// 版本條件同 ffmpeg.cpp（lavc 61.13 起 AVCodec::pix_fmts 被 avcodec_get_supported_config 取代，
// 9.0 已經沒有那個欄位）
const AVPixelFormat* codecPixFmts(const AVCodec* c)
{
    const AVPixelFormat* fmts = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     (const void**)&fmts, nullptr) < 0) {
        fmts = nullptr;
    }
#else
    fmts = c->pix_fmts;
#endif
    return fmts;
}

bool containsPixFmt(const AVPixelFormat* fmts, AVPixelFormat f)
{
    for (int i = 0; fmts != nullptr && fmts[i] != AV_PIX_FMT_NONE; i++) {
        if (fmts[i] == f) {
            return true;
        }
    }
    return false;
}

int buildCandidates(const DecodeBenchOptions& o, const InputInfo& in, QVector<BenchCandidate>& out,
                    bool& autoMode, QString& err)
{
    const QString dec = o.decoder.trimmed();
    autoMode = false;

    auto addSw = [&]() {
        for (const QString& n : swDecoderNames(in.codecId)) {
            const AVCodec* c = avcodec_find_decoder_by_name(n.toUtf8().constData());
            if (c != nullptr && av_codec_is_decoder(c)) {
                out.append(makeCandidate(c, DecKind::Software, nullptr, n));
            }
        }
    };

    const AVCodec* native = avcodec_find_decoder_by_name(nativeDecoderName(in.codecId));

    if (dec.isEmpty() || dec == QLatin1String("auto")) {
        autoMode = true;

        // 1. stateful V4L2 m2m（Frame 的主路徑 L2／L3；ffmpeg.cpp tryInitializeNonHwAccelDecoder）
        const QString m2m = QStringLiteral("%1_v4l2m2m").arg(QLatin1String(nativeDecoderName(in.codecId)));
        const AVCodec* m2mCodec = avcodec_find_decoder_by_name(m2m.toUtf8().constData());
        if (m2mCodec != nullptr && av_codec_is_decoder(m2mCodec)) {
            out.append(makeCandidate(m2mCodec, DecKind::V4l2m2m, nullptr, m2m));
        }

        // 2. hwaccel
        if (native != nullptr) {
            for (const char* name : k_HwPreference) {
                const AVHWDeviceType t = av_hwdevice_find_type_by_name(name);
                if (t == AV_HWDEVICE_TYPE_NONE) {
                    continue;
                }
                const AVCodecHWConfig* cfg = findHwConfig(native, t);
                if (cfg != nullptr) {
                    out.append(makeCandidate(native, DecKind::HwAccel, cfg,
                                             QStringLiteral("%1+%2").arg(QLatin1String(native->name), QLatin1String(name))));
                }
            }
        }

        // 3. SW
        addSw();

        if (out.isEmpty()) {
            err = QStringLiteral("no decoder available for %1 in this FFmpeg build").arg(in.codec);
            return ProbeUtil::kExitCapabilityAbsent;
        }
        return ProbeUtil::kExitOk;
    }

    if (dec == QLatin1String("sw")) {
        addSw();
        if (out.isEmpty()) {
            err = QStringLiteral("no software decoder for %1 in this FFmpeg build").arg(in.codec);
            return ProbeUtil::kExitCapabilityAbsent;
        }
        return ProbeUtil::kExitOk;
    }

    if (dec.startsWith(QLatin1String("hwaccel:"))) {
        const QString typeName = dec.mid(8).trimmed().toLower();
        const AVHWDeviceType t = av_hwdevice_find_type_by_name(typeName.toUtf8().constData());
        if (t == AV_HWDEVICE_TYPE_NONE) {
            err = QStringLiteral("unknown hwaccel type '%1'").arg(typeName);
            return ProbeUtil::kExitBadInput;
        }
        const AVCodecHWConfig* cfg = native != nullptr ? findHwConfig(native, t) : nullptr;
        if (cfg == nullptr) {
            err = QStringLiteral("decoder %1 has no %2 hwaccel in this FFmpeg build")
                      .arg(QLatin1String(nativeDecoderName(in.codecId)), typeName);
            return ProbeUtil::kExitCapabilityAbsent;
        }
        out.append(makeCandidate(native, DecKind::HwAccel, cfg,
                                 QStringLiteral("%1+%2").arg(QLatin1String(native->name), typeName)));
        return ProbeUtil::kExitOk;
    }

    // 指定 FFmpeg decoder 名（例如 hevc_v4l2m2m、hevc、libdav1d）
    const AVCodec* c = avcodec_find_decoder_by_name(dec.toUtf8().constData());
    if (c == nullptr || !av_codec_is_decoder(c)) {
        err = QStringLiteral("decoder '%1' is not registered in this FFmpeg build").arg(dec);
        return ProbeUtil::kExitCapabilityAbsent;
    }
    if (c->id != in.codecId) {
        err = QStringLiteral("decoder '%1' decodes %2, but the input is %3")
                  .arg(dec, QLatin1String(avcodec_get_name(c->id)), in.codec);
        return ProbeUtil::kExitBadInput;
    }
    out.append(makeCandidate(c, classifyNamedDecoder(c), nullptr, dec));
    return ProbeUtil::kExitOk;
}

struct GetFormatState {
    AVPixelFormat hwPixFmt = AV_PIX_FMT_NONE;
    bool wantDrmPrime = false;
    AVPixelFormat chosen = AV_PIX_FMT_NONE;
    QStringList firstOffer;
    bool drmPrimeOffered = false;
    bool hwFmtMissing = false;
    int calls = 0;
};

// 對應 ffmpeg.cpp 的 ffGetFormat：hwaccel 只接受自己的硬體格式（找不到就失敗，不讓 FFmpeg
// 靜默退回軟解）；非 hwaccel decoder 在 --out drm_prime 時選 DRM_PRIME（v4l2m2m 因此設
// output_drm），否則選第一個系統記憶體格式。
enum AVPixelFormat benchGetFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts)
{
    GetFormatState* st = static_cast<GetFormatState*>(ctx->opaque);
    st->calls++;

    const bool record = st->firstOffer.isEmpty();
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (record) {
            st->firstOffer << QString::fromLatin1(pixFmtName(*p));
        }
        if (*p == AV_PIX_FMT_DRM_PRIME) {
            st->drmPrimeOffered = true;
        }
    }

    AVPixelFormat pick = AV_PIX_FMT_NONE;
    if (st->hwPixFmt != AV_PIX_FMT_NONE) {
        for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
            if (*p == st->hwPixFmt) {
                pick = *p;
                break;
            }
        }
        if (pick == AV_PIX_FMT_NONE) {
            st->hwFmtMissing = true;
        }
    }
    else {
        if (st->wantDrmPrime) {
            for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
                if (*p == AV_PIX_FMT_DRM_PRIME) {
                    pick = *p;
                    break;
                }
            }
        }
        if (pick == AV_PIX_FMT_NONE) {
            for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*p);
                if (desc != nullptr && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                    pick = *p;
                    break;
                }
            }
        }
        if (pick == AV_PIX_FMT_NONE) {
            pick = fmts[0];
        }
    }

    st->chosen = pick;
    return pick;
}

struct BenchDecoder {
    BenchCandidate cand;
    AVCodecContext* ctx = nullptr;
    AVBufferRef* hwDevice = nullptr;
    GetFormatState gf;              // ctx->opaque 指向它：BenchDecoder 不可搬移（一律用 unique_ptr）
    QString threadType;
    int threadCount = 0;
    int outputBuffers = -1;
    int captureBuffers = -1;
    QString pixFmtHint;
    QStringList unusedOptions;
    double openMs = 0;

    BenchDecoder() = default;
    BenchDecoder(const BenchDecoder&) = delete;
    BenchDecoder& operator=(const BenchDecoder&) = delete;
    ~BenchDecoder()
    {
        avcodec_free_context(&ctx);
        av_buffer_unref(&hwDevice);
    }
};

struct OpenParams {
    bool outDrm = true;
    int captureBuffers = -1;
    int outputBuffers = -1;
    int width = 0;
    int height = 0;
};

bool openDecoder(BenchDecoder& d, const BenchCandidate& cand, const OpenParams& p, QString& err)
{
    d.cand = cand;
    d.ctx = avcodec_alloc_context3(cand.codec);
    if (d.ctx == nullptr) {
        err = QStringLiteral("avcodec_alloc_context3 failed");
        return false;
    }
    AVCodecContext* ctx = d.ctx;

    // ---- 以下逐項照 ffmpeg.cpp completeInitialization() ----
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    ctx->err_recognition = AV_EF_EXPLODE;

    if (cand.kind == DecKind::Software) {
        const int cpus = SDL_GetCPUCount();
        if (ctx->codec_id == AV_CODEC_ID_AV1) {
            ctx->thread_type = FF_THREAD_FRAME;
            ctx->thread_count = qMin(8, cpus);
            av_opt_set_int(ctx->priv_data, "max_frame_delay", 1, 0);
            d.threadType = QStringLiteral("FRAME");
        }
        else if (ctx->codec_id == AV_CODEC_ID_HEVC) {
            ctx->thread_type = FF_THREAD_FRAME;
            ctx->thread_count = qMin(8, cpus);
            d.threadType = QStringLiteral("FRAME");
        }
        else {
            ctx->thread_type = FF_THREAD_SLICE;
            ctx->thread_count = qMin(MAX_SLICES, cpus);
            d.threadType = QStringLiteral("SLICE");
        }
    }
    else {
        // 硬體解碼不開多執行緒（同 ffmpeg.cpp）
        ctx->thread_count = 1;
        d.threadType = QStringLiteral("none");
    }
    d.threadCount = ctx->thread_count;

    ctx->width = p.width;
    ctx->height = p.height;
    ctx->get_format = benchGetFormat;
    ctx->opaque = &d.gf;
    // app 只有 VR session 用 1/1000000；bench 一律用它：pts 帶幀號，v4l2m2m 經
    // v4l2_buffer.timestamp（微秒）來回換算時才是恆等換算（vr_architecture.md §2.3）
    ctx->pkt_timebase.num = 1;
    ctx->pkt_timebase.den = 1000000;
    ctx->extra_hw_frames = 1;

    d.gf.wantDrmPrime = p.outDrm;
    if (cand.kind == DecKind::HwAccel) {
        const int e = av_hwdevice_ctx_create(&d.hwDevice, cand.hwCfg->device_type, nullptr, nullptr, 0);
        if (e < 0) {
            err = QStringLiteral("av_hwdevice_ctx_create(%1) failed: %2")
                      .arg(QLatin1String(av_hwdevice_get_type_name(cand.hwCfg->device_type)), avErr(e));
            return false;
        }
        ctx->hw_device_ctx = av_buffer_ref(d.hwDevice);
        d.gf.hwPixFmt = cand.hwCfg->pix_fmt;
        if (cand.hwCfg->device_type == AV_HWDEVICE_TYPE_VDPAU) {
            // 同 vdpau.cpp prepareDecoderContext()
            ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
            ctx->hwaccel_flags |= AV_HWACCEL_FLAG_IGNORE_LEVEL;
        }
    }
    else if (cand.kind != DecKind::Software) {
        // 非 hwaccel 的硬體 decoder：和 app 一樣先用 pix_fmt 提示想要的輸出格式
        // （app 走 requiredFormat；v4l2m2m 會把它當 capture 格式的起點）
        const AVPixelFormat want = p.outDrm ? AV_PIX_FMT_DRM_PRIME : AV_PIX_FMT_NV12;
        const AVPixelFormat* fmts = codecPixFmts(cand.codec);
        if (fmts == nullptr || containsPixFmt(fmts, want)) {
            ctx->pix_fmt = want;
            d.pixFmtHint = QString::fromLatin1(pixFmtName(want));
        }
    }

    AVDictionary* opts = nullptr;
    if (cand.kind == DecKind::V4l2m2m) {
        // 同 ffmpeg.cpp：2 個壓縮資料 buffer；4 張參考幀 + Pacer 持有數 + 每個輸出一個工作面
        d.outputBuffers = p.outputBuffers >= 0 ? p.outputBuffers : 2;
        d.captureBuffers = p.captureBuffers >= 0 ? p.captureBuffers : 4 + PACER_MAX_OUTSTANDING_FRAMES + 2;
        av_dict_set_int(&opts, "num_output_buffers", d.outputBuffers, 0);
        av_dict_set_int(&opts, "num_capture_buffers", d.captureBuffers, 0);
    }

    const Clock::time_point t0 = Clock::now();
    const int e = avcodec_open2(ctx, cand.codec, &opts);
    d.openMs = msBetween(t0, Clock::now());

    const AVDictionaryEntry* entry = nullptr;
    while ((entry = av_dict_get(opts, "", entry, AV_DICT_IGNORE_SUFFIX)) != nullptr) {
        d.unusedOptions << QString::fromUtf8(entry->key);
    }
    av_dict_free(&opts);

    if (e < 0) {
        err = QStringLiteral("avcodec_open2 failed: %1").arg(avErr(e));
        return false;
    }
    return true;
}

// 和 app 一樣送非 refcounted 的封包（m_Pkt->data 指向 m_DecodeBuffer）；avcodec_send_packet
// 會自己複製，這裡不必擔心 decoder 改到共用的資料。
void fillPacket(AVPacket* pkt, const BenchPacket& p, int64_t pts, const uint8_t* dataOverride = nullptr)
{
    pkt->data = const_cast<uint8_t*>(dataOverride != nullptr ? dataOverride
                                                             : reinterpret_cast<const uint8_t*>(p.data.constData()));
    pkt->size = p.size;
    pkt->pts = pts;
    pkt->dts = AV_NOPTS_VALUE;
    pkt->flags = p.key ? AV_PKT_FLAG_KEY : 0;
}

// auto 模式的試解：前幾個 AU 內要能出幀（同 app 啟動時用測試幀確認 decoder 真的能動）
bool trialDecode(BenchDecoder& d, const QVector<BenchPacket>& pkts, QString& why)
{
    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    if (pkt == nullptr || f == nullptr) {
        av_packet_free(&pkt);
        av_frame_free(&f);
        why = QStringLiteral("allocation failed");
        return false;
    }

    bool got = false;
    bool failed = false;
    const int n = qMin(8, (int)pkts.size());
    for (int i = 0; i < n && !got && !failed; i++) {
        fillPacket(pkt, pkts[i], i);
        int ret = avcodec_send_packet(d.ctx, pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            why = QStringLiteral("avcodec_send_packet: %1").arg(avErr(ret));
            failed = true;
            break;
        }
        const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(100);
        for (;;) {
            ret = avcodec_receive_frame(d.ctx, f);
            if (ret == 0) {
                got = true;
                break;
            }
            if (ret != AVERROR(EAGAIN)) {
                why = QStringLiteral("avcodec_receive_frame: %1").arg(avErr(ret));
                failed = true;
                break;
            }
            // 還有封包可送就先送（管線式 decoder 要多幾個封包才出第一幀）
            if (i + 1 < n || Clock::now() >= deadline) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    if (!got && !failed) {
        // 還沒出幀：送 EOF 把 decoder 內部排著的幀擠出來（最多等 1 秒）
        avcodec_send_packet(d.ctx, nullptr);
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(1);
        while (Clock::now() < deadline) {
            const int ret = avcodec_receive_frame(d.ctx, f);
            if (ret == 0) {
                got = true;
                break;
            }
            if (ret != AVERROR(EAGAIN)) {
                if (ret != AVERROR_EOF) {
                    why = QStringLiteral("avcodec_receive_frame after EOF: %1").arg(avErr(ret));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!got && why.isEmpty()) {
            why = QStringLiteral("no frame from the first %1 access units").arg(n);
        }
    }

    if (d.gf.hwFmtMissing) {
        got = false;
        why = QStringLiteral("get_format did not offer %1").arg(QLatin1String(pixFmtName(d.gf.hwPixFmt)));
    }
    else if (got && d.gf.hwPixFmt != AV_PIX_FMT_NONE && f->format != d.gf.hwPixFmt) {
        got = false;
        why = QStringLiteral("hwaccel produced %1 instead of %2")
                  .arg(QLatin1String(pixFmtName(f->format)), QLatin1String(pixFmtName(d.gf.hwPixFmt)));
    }

    av_frame_free(&f);
    av_packet_free(&pkt);
    return got;
}

// ---------------------------------------------------------------------------
// luma 取樣與 PSNR（--compare-sw）

struct LumaGrid {
    int width = 0;
    int height = 0;
    int step = 1;
    int gw = 0;
    int gh = 0;
    int samples() const { return gw * gh; }
};

// 等距點取樣（不做平均）：同一批像素在參考幀與受測幀上比較，MSE 是全圖 MSE 的不偏估計，
// 夠判斷「癒合（≥ 40 dB）」，記憶體只要全圖的 1/step²。
LumaGrid makeGrid(int w, int h, int maxSamples)
{
    LumaGrid g;
    g.width = w;
    g.height = h;
    g.step = qMax(1, (int)std::ceil(std::sqrt((double)w * (double)h / (double)qMax(1, maxSamples))));
    g.gw = w / g.step;
    g.gh = h / g.step;
    return g;
}

bool sampleSwFrame(const AVFrame* f, const AVPixFmtDescriptor* desc, const LumaGrid& g, uint8_t* out, QString& why)
{
    if (desc->flags & (AV_PIX_FMT_FLAG_BE | AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) {
        why = QStringLiteral("pixel format %1 not supported for luma compare").arg(QLatin1String(desc->name));
        return false;
    }
    if (f->width < g.width || f->height < g.height) {
        why = QStringLiteral("frame %1x%2 smaller than reference %3x%4")
                  .arg(f->width).arg(f->height).arg(g.width).arg(g.height);
        return false;
    }
    const AVComponentDescriptor& c = desc->comp[0];
    const uint8_t* base = f->data[c.plane];
    if (base == nullptr) {
        why = QStringLiteral("luma plane missing");
        return false;
    }
    const ptrdiff_t linesize = f->linesize[c.plane];
    const int half = g.step / 2;
    for (int gy = 0; gy < g.gh; gy++) {
        const uint8_t* row = base + (ptrdiff_t)(gy * g.step + half) * linesize;
        for (int gx = 0; gx < g.gw; gx++) {
            const uint8_t* px = row + (ptrdiff_t)(gx * g.step + half) * c.step + c.offset;
            unsigned v;
            if (c.depth > 8) {
                v = (((unsigned)px[0] | ((unsigned)px[1] << 8)) >> c.shift) >> (c.depth - 8);
            }
            else {
                v = (unsigned)px[0] >> c.shift;
            }
            *out++ = (uint8_t)qMin(v, 255u);
        }
    }
    return true;
}

#ifdef DECODEBENCH_HAVE_DRM
constexpr uint32_t drmFourcc(char a, char b, char c, char d)
{
    return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) | ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}
constexpr uint64_t k_DrmModLinear = 0;
constexpr uint64_t k_DrmModInvalid = 0x00FFFFFFFFFFFFFFULL;

// v4l2m2m 的 DRM_PRIME 幀沒有 hw_frames_ctx，av_hwframe_transfer_data 用不了：直接 mmap
// dma-buf 讀 luma。只支援線性排列（UBWC 之類的壓縮 modifier 無法用 CPU 解讀，回報原因）。
bool sampleDrmFrame(const AVFrame* f, const LumaGrid& g, uint8_t* out, QString& why)
{
    const AVDRMFrameDescriptor* d = reinterpret_cast<const AVDRMFrameDescriptor*>(f->data[0]);
    if (d == nullptr || d->nb_layers < 1 || d->layers[0].nb_planes < 1) {
        why = QStringLiteral("empty DRM descriptor");
        return false;
    }
    const AVDRMLayerDescriptor& layer = d->layers[0];
    const AVDRMPlaneDescriptor& plane = layer.planes[0];
    if (plane.object_index < 0 || plane.object_index >= d->nb_objects) {
        why = QStringLiteral("bad DRM object index");
        return false;
    }
    const AVDRMObjectDescriptor& obj = d->objects[plane.object_index];
    if (obj.format_modifier != k_DrmModLinear && obj.format_modifier != k_DrmModInvalid) {
        why = QStringLiteral("DRM modifier 0x%1 is not linear; CPU compare unavailable")
                  .arg((qulonglong)obj.format_modifier, 0, 16);
        return false;
    }

    int bytesPerSample;
    const uint32_t fmt = layer.format;
    if (fmt == drmFourcc('N', 'V', '1', '2') || fmt == drmFourcc('N', 'V', '2', '1') ||
            fmt == drmFourcc('Y', 'U', '1', '2') || fmt == drmFourcc('Y', 'V', '1', '2') ||
            fmt == drmFourcc('R', '8', ' ', ' ')) {
        bytesPerSample = 1;
    }
    else if (fmt == drmFourcc('P', '0', '1', '0') || fmt == drmFourcc('P', '0', '1', '2') ||
             fmt == drmFourcc('P', '0', '1', '6')) {
        bytesPerSample = 2;   // 有效位元靠 MSB：取高 8 位
    }
    else {
        why = QStringLiteral("DRM format %1 not supported for luma compare").arg(fourccString(fmt));
        return false;
    }
    if (f->width < g.width || f->height < g.height) {
        why = QStringLiteral("frame %1x%2 smaller than reference").arg(f->width).arg(f->height);
        return false;
    }
    if (obj.fd < 0 || obj.size == 0) {
        why = QStringLiteral("DRM object has no fd/size");
        return false;
    }

    void* map = mmap(nullptr, obj.size, PROT_READ, MAP_SHARED, obj.fd, 0);
    if (map == MAP_FAILED) {
        why = QStringLiteral("mmap(dma-buf) failed: %1").arg(qt_error_string(errno));
        return false;
    }
#ifdef DECODEBENCH_HAVE_DMABUF_SYNC
    struct dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
    ioctl(obj.fd, DMA_BUF_IOCTL_SYNC, &sync);
#endif

    const uint8_t* bytes = static_cast<const uint8_t*>(map);
    const ptrdiff_t pitch = plane.pitch;
    const int half = g.step / 2;
    bool ok = true;
    for (int gy = 0; gy < g.gh && ok; gy++) {
        for (int gx = 0; gx < g.gw; gx++) {
            const ptrdiff_t at = plane.offset + (ptrdiff_t)(gy * g.step + half) * pitch +
                                 (ptrdiff_t)(gx * g.step + half) * bytesPerSample;
            if (at < 0 || (size_t)(at + bytesPerSample) > obj.size) {
                why = QStringLiteral("luma plane exceeds dma-buf size");
                ok = false;
                break;
            }
            *out++ = bytesPerSample == 2 ? bytes[at + 1] : bytes[at];
        }
    }

#ifdef DECODEBENCH_HAVE_DMABUF_SYNC
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
    ioctl(obj.fd, DMA_BUF_IOCTL_SYNC, &sync);
#endif
    munmap(map, obj.size);
    return ok;
}
#endif // DECODEBENCH_HAVE_DRM

// 任意輸出幀 → 取樣後的 8-bit luma。硬體幀有 hw_frames_ctx 時先下載到系統記憶體。
bool sampleFrame(const AVFrame* f, const LumaGrid& g, uint8_t* out, QString& why, AVFrame*& tmp)
{
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)f->format);
    if (desc == nullptr) {
        why = QStringLiteral("unknown pixel format %1").arg(f->format);
        return false;
    }
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
        return sampleSwFrame(f, desc, g, out, why);
    }
#ifdef DECODEBENCH_HAVE_DRM
    // DRM_PRIME 一律直接 mmap（v4l2m2m 的幀沒有 hw_frames_ctx；有的話 transfer 也只是 map+copy）
    if (f->format == AV_PIX_FMT_DRM_PRIME) {
        return sampleDrmFrame(f, g, out, why);
    }
#endif
    if (f->hw_frames_ctx == nullptr) {
        why = QStringLiteral("%1 frame without hw_frames_ctx").arg(QLatin1String(pixFmtName(f->format)));
        return false;
    }
    if (tmp == nullptr) {
        tmp = av_frame_alloc();
        if (tmp == nullptr) {
            why = QStringLiteral("av_frame_alloc failed");
            return false;
        }
    }
    av_frame_unref(tmp);
    const int e = av_hwframe_transfer_data(tmp, f, 0);
    if (e < 0) {
        why = QStringLiteral("av_hwframe_transfer_data: %1").arg(avErr(e));
        return false;
    }
    const AVPixFmtDescriptor* swDesc = av_pix_fmt_desc_get((AVPixelFormat)tmp->format);
    const bool ok = swDesc != nullptr && sampleSwFrame(tmp, swDesc, g, out, why);
    av_frame_unref(tmp);
    return ok;
}

struct CompareState {
    bool active = false;
    QString refDecoder;
    QString error;
    LumaGrid grid;
    std::vector<QByteArray> ref;   // 依檔內幀號 k
    int refFrames = 0;
    int compared = 0;
    int failures = 0;
    QString firstFailure;
    AVFrame* tmp = nullptr;

    CompareState() = default;
    CompareState(const CompareState&) = delete;
    CompareState& operator=(const CompareState&) = delete;
    ~CompareState() { av_frame_free(&tmp); }

    // 回傳 luma PSNR（dB，完全相同記 100）；沒有參考或取樣失敗回 -1
    double psnrFor(int k, const AVFrame* f)
    {
        if (!active || k < 0 || k >= (int)ref.size() || ref[(size_t)k].isEmpty()) {
            return -1;
        }
        QByteArray s(grid.samples(), '\0');
        QString why;
        if (!sampleFrame(f, grid, reinterpret_cast<uint8_t*>(s.data()), why, tmp)) {
            failures++;
            if (firstFailure.isEmpty()) {
                firstFailure = why;
            }
            return -1;
        }
        const uint8_t* a = reinterpret_cast<const uint8_t*>(ref[(size_t)k].constData());
        const uint8_t* b = reinterpret_cast<const uint8_t*>(s.constData());
        const int n = grid.samples();
        double sse = 0;
        for (int i = 0; i < n; i++) {
            const int diff = (int)a[i] - (int)b[i];
            sse += (double)(diff * diff);
        }
        compared++;
        const double mse = sse / (double)qMax(1, n);
        if (mse <= 0) {
            return 100.0;
        }
        return qMin(100.0, 10.0 * std::log10(255.0 * 255.0 / mse));
    }
};

// 參考解碼（不計時）：SW decoder、完整檔案、不丟幀不破損
bool buildReference(CompareState& cs, const QVector<BenchPacket>& pkts, int nf, const InputInfo& in, QString& why)
{
    std::unique_ptr<BenchDecoder> d;
    QString err;
    for (const QString& name : swDecoderNames(in.codecId)) {
        const AVCodec* c = avcodec_find_decoder_by_name(name.toUtf8().constData());
        if (c == nullptr) {
            continue;
        }
        auto candidate = std::make_unique<BenchDecoder>();
        OpenParams op;
        op.outDrm = false;
        op.width = in.width;
        op.height = in.height;
        if (openDecoder(*candidate, makeCandidate(c, DecKind::Software, nullptr, name), op, err)) {
            d = std::move(candidate);
            break;
        }
    }
    if (!d) {
        why = err.isEmpty() ? QStringLiteral("no software decoder for %1").arg(in.codec) : err;
        return false;
    }
    cs.refDecoder = d->cand.label;
    cs.ref.assign((size_t)nf, QByteArray());
    // 三個引數都明確轉成 qint64：qBound<qint64>(int, qint64, int) 會同時命中 Qt 的同型別版與
    // 混型別（Promoted）版而 ambiguous（Qt 6.4／6.10 的 qminmax.h 都一樣）
    const int maxSamples = (int)qBound(qint64(4096), qint64(k_RefBudgetBytes / qMax(1, nf)), qint64(65536));

    AVPacket* pkt = av_packet_alloc();
    AVFrame* f = av_frame_alloc();
    if (pkt == nullptr || f == nullptr) {
        av_packet_free(&pkt);
        av_frame_free(&f);
        why = QStringLiteral("allocation failed");
        return false;
    }

    int nextFifo = 0;   // pts 帶不回來時，依輸出順序對應
    QString sampleWhy;
    auto take = [&](AVFrame* fr) {
        int k = (fr->pts != AV_NOPTS_VALUE && fr->pts >= 0 && fr->pts < nf) ? (int)fr->pts : nextFifo;
        nextFifo = k + 1;
        if (cs.grid.gw == 0) {
            cs.grid = makeGrid(fr->width, fr->height, maxSamples);
        }
        if (k < nf && cs.ref[(size_t)k].isEmpty()) {
            QByteArray s(cs.grid.samples(), '\0');
            if (sampleFrame(fr, cs.grid, reinterpret_cast<uint8_t*>(s.data()), sampleWhy, cs.tmp)) {
                cs.ref[(size_t)k] = s;
                cs.refFrames++;
            }
        }
        av_frame_unref(fr);
    };
    auto drainRef = [&]() -> int {
        for (;;) {
            const int r = avcodec_receive_frame(d->ctx, f);
            if (r != 0) {
                return r;
            }
            take(f);
        }
    };

    for (int k = 0; k < nf; k++) {
        fillPacket(pkt, pkts[k], k);
        for (int retry = 0; retry < 5000; retry++) {
            const int r = avcodec_send_packet(d->ctx, pkt);
            if (r != AVERROR(EAGAIN)) {
                break;   // 其他錯誤：和 app 一樣照樣往下送
            }
            if (drainRef() == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        drainRef();
    }
    avcodec_send_packet(d->ctx, nullptr);
    const Clock::time_point deadline = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < deadline) {
        const int r = drainRef();
        if (r != AVERROR(EAGAIN)) {
            break;   // AVERROR_EOF 或錯誤
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    av_frame_free(&f);
    av_packet_free(&pkt);

    if (cs.refFrames == 0) {
        why = sampleWhy.isEmpty() ? QStringLiteral("reference decode produced no usable frames") : sampleWhy;
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// --map-vulkan（PoC-4）：headless libplacebo，每幀 pl_map_avframe_ex 計時

#ifdef HAVE_LIBPLACEBO_VULKAN
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(60, 26, 100)
// 舊版 FFmpeg 沒有 av_vk_get_optional_device_extensions()：比照 plvk.cpp 的備援清單，
// 只留匯入／匯出相關（這個 device 不做 video decode）
const char* const k_OptionalDeviceExtensions[] = {
    VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
};
#endif

void plLogCallback(void*, enum pl_log_level level, const char* msg)
{
    switch (level) {
    case PL_LOG_FATAL:
    case PL_LOG_ERR:
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_WARN:
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    case PL_LOG_INFO:
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    default:
        SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION, "libplacebo: %s", msg);
        break;
    }
}

class PlMapper {
public:
    PlMapper() = default;
    PlMapper(const PlMapper&) = delete;
    PlMapper& operator=(const PlMapper&) = delete;

    ~PlMapper()
    {
        if (m_Vk != nullptr) {
            for (pl_tex& t : m_Tex) {
                pl_tex_destroy(m_Vk->gpu, &t);
            }
        }
        pl_vulkan_destroy(&m_Vk);
        pl_vk_inst_destroy(&m_Inst);
        pl_log_destroy(&m_Log);
        // 不卸載 Vulkan loader：行程接著就 _Exit，卸載反而可能踩到 layer 的解構順序
    }

    bool init(bool verbose, QString& why)
    {
        // 自己載 loader 取 vkGetInstanceProcAddr：probe 路徑沒有初始化 SDL video，
        // 不能用 SDL_Vulkan_GetVkGetInstanceProcAddr()
#ifdef Q_OS_WIN32
        m_VkLib = SDL_LoadObject("vulkan-1.dll");
#else
        m_VkLib = SDL_LoadObject("libvulkan.so.1");
#endif
        if (m_VkLib == nullptr) {
            why = QStringLiteral("cannot load the Vulkan loader: %1").arg(QString::fromUtf8(SDL_GetError()));
            return false;
        }
        auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_LoadFunction(m_VkLib, "vkGetInstanceProcAddr"));
        if (gipa == nullptr) {
            why = QStringLiteral("vkGetInstanceProcAddr not found");
            return false;
        }

        pl_log_params logParams = pl_log_default_params;
        logParams.log_cb = plLogCallback;
        logParams.log_level = verbose ? PL_LOG_DEBUG : PL_LOG_WARN;
        m_Log = pl_log_create(PL_API_VER, &logParams);

        pl_vk_inst_params instParams = pl_vk_inst_default_params;
        instParams.get_proc_addr = gipa;
        m_Inst = pl_vk_inst_create(m_Log, &instParams);
        if (m_Inst == nullptr) {
            why = QStringLiteral("pl_vk_inst_create() failed");
            return false;
        }

        // 不給 surface（headless）：PoC-4 只量映射，不 present。opt_extensions 照 plvk.cpp，
        // 讓 device 的擴充組合和串流時一致
        pl_vulkan_params vkParams = pl_vulkan_default_params;
        vkParams.instance = m_Inst->instance;
        vkParams.get_proc_addr = m_Inst->get_proc_addr;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 26, 100)
        vkParams.opt_extensions = av_vk_get_optional_device_extensions(&vkParams.num_opt_extensions);
#else
        vkParams.opt_extensions = k_OptionalDeviceExtensions;
        vkParams.num_opt_extensions = (int)SDL_arraysize(k_OptionalDeviceExtensions);
#endif
        m_Vk = pl_vulkan_create(m_Log, &vkParams);
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(60, 26, 100)
        av_free((void*)vkParams.opt_extensions);
#endif
        if (m_Vk == nullptr) {
            why = QStringLiteral("pl_vulkan_create() failed");
            return false;
        }

        auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            m_Inst->get_proc_addr(m_Inst->instance, "vkGetPhysicalDeviceProperties"));
        if (getProps != nullptr) {
            VkPhysicalDeviceProperties props = {};
            getProps(m_Vk->phys_device, &props);
            m_DeviceName = QString::fromUtf8(props.deviceName);
        }
        m_ApiVersion = QStringLiteral("%1.%2.%3")
                           .arg(VK_API_VERSION_MAJOR(m_Vk->api_version))
                           .arg(VK_API_VERSION_MINOR(m_Vk->api_version))
                           .arg(VK_API_VERSION_PATCH(m_Vk->api_version));
        return true;
    }

    const QString& deviceName() const { return m_DeviceName; }

    void mapTimed(const AVFrame* f)
    {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)f->format);
        const bool hw = desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
        // DRM_PRIME 直接匯入 dma-buf；VAAPI 經 av_hwframe_map 轉 DRM 再匯入；系統記憶體幀走上傳。
        // AV_PIX_FMT_VULKAN 要和 decoder 共用同一個 VkDevice（這裡是獨立 device），D3D11 等不支援
        if (hw && f->format != AV_PIX_FMT_DRM_PRIME && f->format != AV_PIX_FMT_VAAPI) {
            if (m_Unsupported++ == 0) {
                m_FirstUnsupported = QString::fromLatin1(pixFmtName(f->format));
            }
            return;
        }

        pl_frame mapped = {};
        pl_avframe_params params = {};
        params.frame = f;
        params.tex = m_Tex;
        const Clock::time_point a = Clock::now();
        const bool ok = pl_map_avframe_ex(m_Vk->gpu, &mapped, &params);
        const Clock::time_point b = Clock::now();
        if (!ok) {
            // 失敗時 pl_map_avframe_ex 已自己 unmap
            m_Failures++;
            return;
        }
        pl_gpu_finish(m_Vk->gpu);
        const Clock::time_point c = Clock::now();
        pl_unmap_avframe(m_Vk->gpu, &mapped);

        m_MapMs.push_back(msBetween(a, b));
        m_MapFinishMs.push_back(msBetween(a, c));
    }

    Dist mapDist() const { return makeDist(m_MapMs); }

    QJsonObject json() const
    {
        QJsonObject j;
        j[QStringLiteral("available")] = true;
        j[QStringLiteral("device")] = m_DeviceName;
        j[QStringLiteral("apiVersion")] = m_ApiVersion;
        j[QStringLiteral("mapped")] = (int)m_MapMs.size();
        j[QStringLiteral("mapMs")] = distJson(makeDist(m_MapMs));
        j[QStringLiteral("mapPlusGpuFinishMs")] = distJson(makeDist(m_MapFinishMs));
        j[QStringLiteral("failures")] = m_Failures;
        j[QStringLiteral("unsupported")] = m_Unsupported;
        if (!m_FirstUnsupported.isEmpty()) {
            j[QStringLiteral("unsupportedFormat")] = m_FirstUnsupported;
        }
        return j;
    }

    int failures() const { return m_Failures; }
    int unsupported() const { return m_Unsupported; }

private:
    void* m_VkLib = nullptr;
    pl_log m_Log = nullptr;
    pl_vk_inst m_Inst = nullptr;
    pl_vulkan m_Vk = nullptr;
    pl_tex m_Tex[4] = {};
    QString m_DeviceName;
    QString m_ApiVersion;
    std::vector<double> m_MapMs;
    std::vector<double> m_MapFinishMs;
    int m_Failures = 0;
    int m_Unsupported = 0;
    QString m_FirstUnsupported;
};
#endif // HAVE_LIBPLACEBO_VULKAN

// ---------------------------------------------------------------------------
// 首幀描述

#ifdef DECODEBENCH_HAVE_DRM
QJsonObject drmJson(const AVDRMFrameDescriptor* d, QString& summary)
{
    QJsonObject j;
    QJsonArray objects;
    for (int i = 0; i < d->nb_objects && i < AV_DRM_MAX_PLANES; i++) {
        QJsonObject o;
        o[QStringLiteral("size")] = (qint64)d->objects[i].size;
        o[QStringLiteral("modifier")] = QStringLiteral("0x%1").arg((qulonglong)d->objects[i].format_modifier, 16, 16, QLatin1Char('0'));
        objects.append(o);
    }
    QJsonArray layers;
    QStringList formats;
    QStringList pitches;
    for (int l = 0; l < d->nb_layers && l < AV_DRM_MAX_PLANES; l++) {
        const AVDRMLayerDescriptor& layer = d->layers[l];
        QJsonObject lo;
        lo[QStringLiteral("format")] = fourccString(layer.format);
        // 每個 layer 都列：VAAPI 匯出的 NV12 是 R8 + GR88 兩個 layer，只看第一個會誤判成單平面 R8
        formats << fourccString(layer.format).trimmed();
        QJsonArray planes;
        for (int p = 0; p < layer.nb_planes && p < AV_DRM_MAX_PLANES; p++) {
            QJsonObject po;
            po[QStringLiteral("object")] = layer.planes[p].object_index;
            po[QStringLiteral("offset")] = (qint64)layer.planes[p].offset;
            po[QStringLiteral("pitch")] = (qint64)layer.planes[p].pitch;
            planes.append(po);
            pitches << QString::number((qint64)layer.planes[p].pitch);
        }
        lo[QStringLiteral("planes")] = planes;
        layers.append(lo);
    }
    j[QStringLiteral("nbObjects")] = d->nb_objects;
    j[QStringLiteral("objects")] = objects;
    j[QStringLiteral("nbLayers")] = d->nb_layers;
    j[QStringLiteral("layers")] = layers;

    const QString fmt = formats.isEmpty() ? QStringLiteral("?") : formats.join(QLatin1Char('+'));
    const qulonglong mod = d->nb_objects > 0 ? (qulonglong)d->objects[0].format_modifier : 0;
    summary = QStringLiteral("%1/mod=0x%2/objs=%3/pitch=%4")
                  .arg(fmt)
                  .arg(mod, 0, 16)
                  .arg(d->nb_objects)
                  .arg(pitches.join(QLatin1Char(',')));
    return j;
}
#endif

QJsonObject describeFrame(const AVFrame* f, QString& drmSummary)
{
    QJsonObject j;
    j[QStringLiteral("pixFmt")] = QString::fromLatin1(pixFmtName(f->format));
    j[QStringLiteral("width")] = f->width;
    j[QStringLiteral("height")] = f->height;
    j[QStringLiteral("key")] = (f->flags & AV_FRAME_FLAG_KEY) != 0;
    const char* range = av_color_range_name(f->color_range);
    const char* space = av_color_space_name(f->colorspace);
    j[QStringLiteral("colorRange")] = QString::fromLatin1(range ? range : "?");
    j[QStringLiteral("colorSpace")] = QString::fromLatin1(space ? space : "?");
    if (f->hw_frames_ctx != nullptr) {
        const AVHWFramesContext* fc = reinterpret_cast<const AVHWFramesContext*>(f->hw_frames_ctx->data);
        j[QStringLiteral("swFormat")] = QString::fromLatin1(pixFmtName(fc->sw_format));
        j[QStringLiteral("hwPoolSize")] = fc->initial_pool_size;
    }
    drmSummary = QStringLiteral("-");
#ifdef DECODEBENCH_HAVE_DRM
    if (f->format == AV_PIX_FMT_DRM_PRIME && f->data[0] != nullptr) {
        j[QStringLiteral("drm")] = drmJson(reinterpret_cast<const AVDRMFrameDescriptor*>(f->data[0]), drmSummary);
        j[QStringLiteral("drmVia")] = QStringLiteral("decoder");
    }
    else if (f->hw_frames_ctx != nullptr) {
        // 例如 VAAPI（S1 的 AMD builder）：只為描述而匯出一次 DRM layout，不計時
        AVFrame* m = av_frame_alloc();
        if (m != nullptr) {
            m->format = AV_PIX_FMT_DRM_PRIME;
            const int e = av_hwframe_map(m, f, AV_HWFRAME_MAP_READ);
            if (e == 0 && m->data[0] != nullptr) {
                j[QStringLiteral("drm")] = drmJson(reinterpret_cast<const AVDRMFrameDescriptor*>(m->data[0]), drmSummary);
                j[QStringLiteral("drmVia")] = QStringLiteral("av_hwframe_map");
            }
            else {
                j[QStringLiteral("drmMapError")] = avErr(e);
            }
            av_frame_free(&m);
        }
    }
#endif
    return j;
}

// ---------------------------------------------------------------------------
// 計時迴圈

struct RunConfig {
    int fps = 0;
    int nf = 0;          // 每輪送幾個 AU
    int loops = 1;
    int warmup = 0;
    int hold = 0;
    QSet<int> drop;
    int dropEvery = 0;
    QSet<int> corrupt;
    bool flushOnError = false;
    bool verbose = false;
};

enum FrameStatus : uint8_t {
    kPending = 0,
    kSent,
    kDropped,      // --drop-*：刻意不送
    kSendFailed,
    kFlushed,      // --flush-on-error 清掉時還在 decoder 裡
    kReceived,
    kSkipped,      // 送了但 decoder 一直沒吐出來
};

struct BenchEvent {
    int g = 0;
    QString type;     // drop | corrupt | error
    QString detail;
};

class BenchRun {
public:
    BenchRun(BenchDecoder& dec, const QVector<BenchPacket>& pkts, const RunConfig& cfg, CompareState* cmp)
        : m_Dec(dec), m_Pkts(pkts), m_Cfg(cfg), m_Cmp(cmp)
    {
    }

    BenchRun(const BenchRun&) = delete;
    BenchRun& operator=(const BenchRun&) = delete;

    ~BenchRun()
    {
        releaseAll();
    }

#ifdef HAVE_LIBPLACEBO_VULKAN
    void setMapper(PlMapper* mapper) { m_Mapper = mapper; }
#endif

    void execute()
    {
        // benchMain 已經擋掉超過 k_MaxScheduledFrames 的組合；這裡用 qint64 再驗一次，乘法本身不會溢位
        const qint64 total = (qint64)m_Cfg.nf * m_Cfg.loops;
        if (total < 0 || total > k_MaxScheduledFrames) {
            abortRun(QStringLiteral("scheduled frame count %1 is out of range").arg(total));
            return;
        }
        m_Total = (int)total;
        try {
            m_Status.assign((size_t)m_Total, kPending);
            m_SendMs.assign((size_t)m_Total, -1.0);
            m_OutMs.assign((size_t)m_Total, -1.0);
            m_OutState.assign((size_t)m_Total, (int8_t)-1);
            m_Psnr.assign((size_t)m_Total, -1.0f);
        }
        catch (const std::bad_alloc&) {
            // 縱深防禦：記憶體本來就吃緊時照樣中止並寫出 JSON 與結束碼，不要 std::terminate。
            // m_Total 歸零，之後的統計不會去索引配置到一半的表
            m_Total = 0;
            abortRun(QStringLiteral("out of memory allocating per-frame tables for %1 frames").arg(total));
            return;
        }
        m_IntervalMs = m_Cfg.fps > 0 ? 1000.0 / m_Cfg.fps : 0.0;
        m_StallThresholdMs = m_Cfg.fps > 0 ? 3.0 * m_IntervalMs : 100.0;

        m_Pkt = av_packet_alloc();
        if (m_Pkt == nullptr) {
            abortRun(QStringLiteral("av_packet_alloc failed"));
            return;
        }

        m_T0 = Clock::now();
        m_LastProgress = m_T0;

        for (int g = 0; g < m_Total && !m_Aborted; g++) {
            const int k = g % m_Cfg.nf;
            if (m_Cfg.fps > 0) {
                const auto offset = std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double, std::milli>(g * m_IntervalMs));
                waitUntil(m_T0 + offset);
                if (m_Aborted) {
                    break;
                }
            }

            if (isDropped(k)) {
                m_Status[(size_t)g] = kDropped;
                m_Dropped++;
                inject(g, "drop");
                continue;
            }

            const BenchPacket& p = m_Pkts[k];
            const uint8_t* data = nullptr;
            if (m_Cfg.corrupt.contains(k)) {
                m_Scratch = p.data;
                corruptPayload(m_Scratch, p.size);
                data = reinterpret_cast<const uint8_t*>(m_Scratch.constData());
                m_Corrupted++;
                inject(g, "corrupt");
            }
            fillPacket(m_Pkt, p, g, data);
            sendOne(g);
            progress(g);
        }

        if (!m_Aborted) {
            tail();
        }

        // decoder 到最後都沒吐出來的幀
        for (int g : m_Inflight) {
            m_Status[(size_t)g] = kSkipped;
            m_Skipped++;
            m_NeverOutput++;
        }
        m_Inflight.clear();
        if (m_InStall) {
            m_StallMs += nowMs() - m_StallStartMs;
            m_InStall = false;
        }
        m_WallMs = nowMs();
        releaseAll();
    }

    // ---- 結果 ----
    int total() const { return m_Total; }
    bool aborted() const { return m_Aborted; }
    const QString& abortReason() const { return m_AbortReason; }
    int framesOut() const { return m_FramesOut; }
    Dist latency() const { return makeDist(m_Latency); }
    const QJsonObject& firstFrame() const { return m_FirstFrame; }
    const QString& firstFrameFormat() const { return m_FirstFrameFormat; }
    const QString& drmSummary() const { return m_DrmSummary; }
    int firstWidth() const { return m_FirstWidth; }
    int firstHeight() const { return m_FirstHeight; }
    int errFlagFrames() const { return m_ErrFlagFrames; }
    int stalls() const { return m_Stalls; }
    int eagainMax() const { return m_EagainMax; }
    double throughputFps() const
    {
        const double span = m_LastCountedOutMs - m_FirstCountedSendMs;
        return (m_CountedFrames > 1 && span > 0) ? (double)m_CountedFrames * 1000.0 / span : 0.0;
    }

    QJsonObject timingJson() const
    {
        QJsonObject j;
        j[QStringLiteral("pollUs")] = k_PollUs;
        j[QStringLiteral("fps")] = m_Cfg.fps;
        j[QStringLiteral("intervalMs")] = round3(m_IntervalMs);
        j[QStringLiteral("stallThresholdMs")] = round3(m_StallThresholdMs);
        j[QStringLiteral("latencyMs")] = distJson(makeDist(m_Latency));
        j[QStringLiteral("sendBlockMs")] = distJson(makeDist(m_SendBlock));
        j[QStringLiteral("firstFrameMs")] = m_FirstFrameMs >= 0 ? QJsonValue(round3(m_FirstFrameMs)) : QJsonValue();
        j[QStringLiteral("throughputFps")] = round3(throughputFps());
        j[QStringLiteral("wallMs")] = round3(m_WallMs);
        j[QStringLiteral("countedFrames")] = m_CountedFrames;
        j[QStringLiteral("stallMs")] = round3(m_StallMs);
        j[QStringLiteral("maxGapMs")] = round3(m_MaxGapMs);
        return j;
    }

    QJsonObject countersJson() const
    {
        QJsonObject j;
        j[QStringLiteral("scheduled")] = m_Total;
        j[QStringLiteral("sent")] = m_Sent;
        j[QStringLiteral("dropped")] = m_Dropped;
        j[QStringLiteral("corrupted")] = m_Corrupted;
        j[QStringLiteral("framesOut")] = m_FramesOut;
        j[QStringLiteral("framesAtEof")] = m_FramesAtEof;
        j[QStringLiteral("sendErrors")] = m_SendErrors;
        j[QStringLiteral("recvErrors")] = m_RecvErrors;
        j[QStringLiteral("recvErrorCalls")] = m_RecvErrorCalls;
        if (!m_FirstSendError.isEmpty()) {
            j[QStringLiteral("firstSendError")] = m_FirstSendError;
        }
        if (!m_FirstRecvError.isEmpty()) {
            j[QStringLiteral("firstRecvError")] = m_FirstRecvError;
        }
        j[QStringLiteral("errFlagFrames")] = m_ErrFlagFrames;
        j[QStringLiteral("errFlagsOr")] = m_ErrFlagsOr;
        j[QStringLiteral("corruptFlagFrames")] = m_CorruptFlagFrames;
        j[QStringLiteral("stalls")] = m_Stalls;
        j[QStringLiteral("eagainMax")] = m_EagainMax;
        j[QStringLiteral("sendEagain")] = m_SendEagain;
        j[QStringLiteral("sendEagainMax")] = m_SendEagainMax;
        j[QStringLiteral("ptsMismatch")] = m_PtsMismatch;
        j[QStringLiteral("ptsNonMonotonic")] = m_PtsNonMonotonic;
        j[QStringLiteral("skipped")] = m_Skipped;
        j[QStringLiteral("neverOutput")] = m_NeverOutput;
        j[QStringLiteral("orphanFrames")] = m_OrphanFrames;
        j[QStringLiteral("flushes")] = m_Flushes;
        j[QStringLiteral("lostInFlush")] = m_LostInFlush;
        j[QStringLiteral("heldMax")] = m_Cfg.hold;
        return j;
    }

    // 事件的癒合分析。視窗規則：
    //  - 注入事件（drop／corrupt）的視窗到下一個注入事件（或結尾）為止，不被中間的 error 事件切斷。
    //    onFrame() 的錯誤區段遇到第一張乾淨幀就關，錯誤旗標斷斷續續時同一次注入的後果會另開
    //    error 事件；落在注入視窗內、PSNR 癒合點之前（沒癒合就是整個視窗）的 error 事件在這裡
    //    併回注入事件（JSON 的 mergedInto），不另算 selfHealed／needIdr
    //  - 癒合之後才出現的 error 事件自己算（注入事件的視窗照舊到下一個注入事件，兩者重疊）。
    //    不在那裡截斷注入視窗：截斷只會讓癒合點附近沒有 PSNR 取樣時誤判成沒癒合
    //  - error 事件的視窗到下一個事件（任何種類）為止
    //  - 視窗被下一個事件截斷時 PSNR 還沒回到門檻：不知道再等下去會不會自己好 → idrNeeded 記
    //    null、truncated=true，另計 inconclusive，不算進 needIdr（例如 --drop-every 很密）
    QJsonArray analyzeEvents(QJsonObject& summary, QStringList& lines) const
    {
        QJsonArray arr;
        // selfHealed：沒靠 keyframe 就回到 ≥ 40 dB；needIdr：要 keyframe 才恢復，或到結尾都沒恢復
        int selfHealed = 0, needIdr = 0, inconclusive = 0, merged = 0, healMax = -1, omittedLines = 0;
        double healSum = 0;
        const bool havePsnr = m_Cmp != nullptr && m_Cmp->active;
        const int n = (int)m_Events.size();

        // 事件大致照 g 的順序加入，但 frame-flags 事件的 g 是輸出幀號，輸出亂序時可能倒退：先排序
        std::vector<int> order((size_t)n);
        for (int i = 0; i < n; i++) {
            order[(size_t)i] = i;
        }
        std::stable_sort(order.begin(), order.end(),
                         [this](int a, int b) { return m_Events[a].g < m_Events[b].g; });
        std::vector<bool> absorbedFlag((size_t)n, false);
        const auto isError = [this](int idx) { return m_Events[idx].type == QLatin1String("error"); };

        // PSNR 癒合：從視窗尾端往回找「最後一段連續 ≥ 40 dB」的起點；沒有 PSNR 的幀
        // （沒吐出來或取樣失敗）不打斷連續段。anyPsnr＝視窗內至少一幀有 PSNR
        const auto healStart = [this, havePsnr](int gStart, int gEnd, bool& anyPsnr) {
            int healG = -1;
            anyPsnr = false;
            if (!havePsnr) {
                return healG;
            }
            for (int g = qMin(gEnd, m_Total) - 1; g >= gStart; g--) {
                const float p = m_Psnr[(size_t)g];
                if (p < 0) {
                    continue;
                }
                anyPsnr = true;
                if (p >= k_HealPsnrDb) {
                    healG = g;
                }
                else {
                    break;
                }
            }
            return healG;
        };

        for (int i = 0; i < n; i++) {
            const int idx = order[(size_t)i];
            if (absorbedFlag[(size_t)idx]) {
                continue;   // 已經在所屬的注入事件那一輪輸出
            }
            const BenchEvent& ev = m_Events[idx];
            const bool injected = !isError(idx);
            const int gStart = ev.type == QLatin1String("drop") ? ev.g + 1 : ev.g;

            // 視窗終點：注入事件看下一個注入事件，error 事件看下一個事件
            int next = i + 1;
            while (injected && next < n && isError(order[(size_t)next])) {
                next++;
            }
            const int gEnd = next < n ? qMax(gStart, m_Events[order[(size_t)next]].g) : m_Total;
            const bool cutByEvent = next < n;

            bool anyPsnr = false;
            const int healG = healStart(gStart, gEnd, anyPsnr);
            // 注入視窗內、癒合點之前的 error 事件併進來（已排序：遇到癒合點之後的就停）
            QVector<int> absorbed;
            for (int m = i + 1; m < next; m++) {
                const int eIdx = order[(size_t)m];
                const int eg = m_Events[eIdx].g;
                if (eg >= gEnd || (healG >= 0 && eg >= healG)) {
                    break;
                }
                absorbed.append(eIdx);
                absorbedFlag[(size_t)eIdx] = true;
            }
            merged += (int)absorbed.size();

            int errFrames = 0, missing = 0, outputs = 0, lastErrG = -1;
            double maxGap = 0, prevOut = -1, psnrMin = -1;
            for (int g = gStart; g < gEnd && g < m_Total; g++) {
                const uint8_t st = m_Status[(size_t)g];
                if (st == kReceived) {
                    outputs++;
                    if (m_OutState[(size_t)g] > 0) {
                        errFrames++;
                        lastErrG = g;
                    }
                    if (prevOut >= 0) {
                        maxGap = qMax(maxGap, m_OutMs[(size_t)g] - prevOut);
                    }
                    prevOut = m_OutMs[(size_t)g];
                    const float p = m_Psnr[(size_t)g];
                    if (p >= 0 && (psnrMin < 0 || p < psnrMin)) {
                        psnrMin = p;
                    }
                }
                else if (st != kDropped) {
                    missing++;
                }
            }

            const int healFrames = healG >= 0 ? healG - gStart : -1;
            const int healByFlags = lastErrG >= 0 ? lastErrG + 1 - gStart : 0;
            // 癒合之前（事件幀之後）有送出 keyframe：是靠 IDR（例如 --loop 下一輪開頭）才恢復，
            // 不是 decoder／intra refresh 自己好的 → 串流時一樣要 IDR
            bool healedByKeyframe = false;
            for (int g = ev.g + 1; healG >= 0 && g <= healG; g++) {
                if (m_Status[(size_t)g] != kDropped && m_Pkts[g % m_Cfg.nf].key) {
                    healedByKeyframe = true;
                    break;
                }
            }
            const bool idrNeeded = healFrames < 0 || healedByKeyframe;
            const bool judged = havePsnr && anyPsnr;
            // 還沒癒合就被下一個事件截斷：判不出來
            const bool truncated = judged && healG < 0 && cutByEvent;

            if (judged) {
                if (truncated) {
                    inconclusive++;
                }
                else if (!idrNeeded) {
                    selfHealed++;
                    healSum += healFrames;
                    healMax = qMax(healMax, healFrames);
                }
                else {
                    needIdr++;
                }
            }

            if (arr.size() < k_MaxJsonEvents) {
                QJsonObject o;
                o[QStringLiteral("g")] = ev.g;
                o[QStringLiteral("k")] = ev.g % m_Cfg.nf;
                o[QStringLiteral("loop")] = ev.g / m_Cfg.nf;
                o[QStringLiteral("type")] = ev.type;
                if (!ev.detail.isEmpty()) {
                    o[QStringLiteral("detail")] = ev.detail;
                }
                o[QStringLiteral("windowFrames")] = qMax(0, qMin(gEnd, m_Total) - gStart);
                o[QStringLiteral("outputs")] = outputs;
                o[QStringLiteral("errFrames")] = errFrames;
                o[QStringLiteral("missing")] = missing;
                o[QStringLiteral("maxGapMs")] = round3(maxGap);
                o[QStringLiteral("healFramesByFlags")] = healByFlags;
                if (!absorbed.isEmpty()) {
                    o[QStringLiteral("mergedErrors")] = (int)absorbed.size();
                }
                if (judged) {
                    o[QStringLiteral("healFrames")] = healFrames;
                    o[QStringLiteral("healedByKeyframe")] = healedByKeyframe;
                    o[QStringLiteral("idrNeeded")] = truncated ? QJsonValue() : QJsonValue(idrNeeded);
                    o[QStringLiteral("truncated")] = truncated;
                    o[QStringLiteral("psnrMin")] = round3(psnrMin);
                }
                else {
                    o[QStringLiteral("healFrames")] = QJsonValue();
                    o[QStringLiteral("idrNeeded")] = QJsonValue();
                }
                arr.append(o);
            }
            // 被併進來的 error 事件緊接在注入事件之後列出，只記歸屬，不另算視窗
            for (int a : absorbed) {
                if (arr.size() >= k_MaxJsonEvents) {
                    break;
                }
                const BenchEvent& me = m_Events[a];
                QJsonObject o;
                o[QStringLiteral("g")] = me.g;
                o[QStringLiteral("k")] = me.g % m_Cfg.nf;
                o[QStringLiteral("loop")] = me.g / m_Cfg.nf;
                o[QStringLiteral("type")] = me.type;
                if (!me.detail.isEmpty()) {
                    o[QStringLiteral("detail")] = me.detail;
                }
                o[QStringLiteral("mergedInto")] = ev.g;
                arr.append(o);
            }

            if (lines.size() < k_MaxEventLines) {
                const QString heal = judged ? QString::number(healFrames) : QStringLiteral("n/a");
                const QString idr = !judged ? QStringLiteral("n/a")
                                  : truncated ? QStringLiteral("inconclusive")
                                  : QString::number(idrNeeded ? 1 : 0);
                QString line = QStringLiteral("bench: %1=%2 errFrames=%3 missing=%4 stallMs=%5 healFrames=%6 (psnr>%7dB) idrNeeded=%8")
                                   .arg(ev.type).arg(ev.g % m_Cfg.nf).arg(errFrames).arg(missing)
                                   .arg(maxGap, 0, 'f', 1).arg(heal).arg((int)k_HealPsnrDb).arg(idr);
                if (!absorbed.isEmpty()) {
                    line += QStringLiteral(" merged=%1").arg((int)absorbed.size());
                }
                lines << line;
            }
            else {
                omittedLines++;
            }
        }
        if (omittedLines > 0) {
            lines << QStringLiteral("bench: ... %1 more event(s) in the JSON").arg(omittedLines);
        }

        summary[QStringLiteral("events")] = n;
        summary[QStringLiteral("merged")] = merged;
        summary[QStringLiteral("jsonTruncated")] = n > k_MaxJsonEvents;
        if (havePsnr) {
            summary[QStringLiteral("selfHealed")] = selfHealed;
            summary[QStringLiteral("needIdr")] = needIdr;
            summary[QStringLiteral("inconclusive")] = inconclusive;
            summary[QStringLiteral("healMax")] = healMax;
            summary[QStringLiteral("healMean")] = selfHealed > 0 ? round3(healSum / selfHealed) : 0.0;
        }
        return arr;
    }

    QJsonObject psnrSummary() const
    {
        QJsonObject j;
        double minv = -1, sum = 0;
        int n = 0, below = 0;
        for (float p : m_Psnr) {
            if (p < 0) {
                continue;
            }
            n++;
            sum += p;
            if (minv < 0 || p < minv) {
                minv = p;
            }
            if (p < k_HealPsnrDb) {
                below++;
            }
        }
        j[QStringLiteral("frames")] = n;
        j[QStringLiteral("min")] = n > 0 ? QJsonValue(round3(minv)) : QJsonValue();
        j[QStringLiteral("mean")] = n > 0 ? QJsonValue(round3(sum / n)) : QJsonValue();
        j[QStringLiteral("below40")] = below;
        return j;
    }

    int eventCount() const { return (int)m_Events.size(); }

private:
    double nowMs() const { return msBetween(m_T0, Clock::now()); }

    bool isDropped(int k) const
    {
        if (m_Cfg.drop.contains(k)) {
            return true;
        }
        // 每 N 幀丟一幀（k = N-1, 2N-1, …）；每輪開頭的 IDR 永遠不丟
        return m_Cfg.dropEvery > 0 && k > 0 && ((k + 1) % m_Cfg.dropEvery) == 0;
    }

    // 破損注入：翻轉 payload 中段的位元組（避開開頭的 NAL／OBU 標頭），模擬封包內容損壞
    static void corruptPayload(QByteArray& buf, int size)
    {
        const int len = qMax(1, qMin(64, size / 4));
        const int start = qBound(0, size / 2 - len / 2, qMax(0, size - len));
        for (int i = 0; i < len && start + i < size; i++) {
            buf[start + i] = (char)(buf[start + i] ^ 0x5A);
        }
    }

    void abortRun(const QString& reason)
    {
        if (!m_Aborted) {
            m_Aborted = true;
            m_AbortReason = reason;
        }
    }

    void addEvent(int g, const char* type, const char* detail)
    {
        BenchEvent ev;
        ev.g = g;
        ev.type = QString::fromLatin1(type);
        ev.detail = QString::fromLatin1(detail);
        m_Events.append(ev);
    }

    void inject(int g, const char* type)
    {
        // 注入的事件本身就開一個錯誤區段：之後 decoder 回報的錯誤歸到這個事件，不另開。
        // 區段遇到第一張乾淨幀就關（--flush-on-error 每區段只 flush 一次靠它）；之後斷斷續續
        // 冒出的錯誤會另開 error 事件，由 analyzeEvents() 依 PSNR 癒合點併回這個注入事件
        m_EpisodeOpen = true;
        m_EpisodeFlushed = false;
        m_EpisodeStartG = g;
        addEvent(g, type, "");
    }

    void onError(int g, const char* reason)
    {
        if (!m_EpisodeOpen) {
            m_EpisodeOpen = true;
            m_EpisodeFlushed = false;
            m_EpisodeStartG = g;
            addEvent(g, "error", reason);
        }
        if (m_Cfg.flushOnError && !m_EpisodeFlushed) {
            // G-rc 情況 ii：出錯後 flush，看 decoder 自己能不能恢復。每個錯誤區段只 flush 一次，
            // 否則 flush 後缺參考的幀又帶錯誤旗標，會一直 flush 下去
            m_EpisodeFlushed = true;
            avcodec_flush_buffers(m_Dec.ctx);
            m_Flushes++;
            for (int x : m_Inflight) {
                m_Status[(size_t)x] = kFlushed;
                m_LostInFlush++;
            }
            m_Inflight.clear();
        }
    }

    void waitUntil(Clock::time_point target)
    {
        for (;;) {
            drain();
            if (m_Aborted) {
                return;
            }
            const Clock::time_point now = Clock::now();
            if (now >= target) {
                return;
            }
            // 最後一小段改 yield 自旋，送出時間才準
            if (target - now > std::chrono::microseconds(2 * k_PollUs)) {
                sleepPoll();
            }
            else {
                std::this_thread::yield();
            }
        }
    }

    void sendOne(int g)
    {
        const Clock::time_point tStart = Clock::now();
        m_SendMs[(size_t)g] = msBetween(m_T0, tStart);
        double blockMs = 0;
        int streak = 0;
        int ret;
        for (;;) {
            const Clock::time_point a = Clock::now();
            ret = avcodec_send_packet(m_Dec.ctx, m_Pkt);
            blockMs += msBetween(a, Clock::now());
            if (ret != AVERROR(EAGAIN)) {
                break;
            }
            m_SendEagain++;
            m_SendEagainMax = qMax(m_SendEagainMax, ++streak);
            // 輸入佇列滿：照 API 先收幀才能再送
            drain();
            if (m_Aborted) {
                break;
            }
            if (msBetween(tStart, Clock::now()) > k_DeadlockMs) {
                abortRun(QStringLiteral("avcodec_send_packet kept returning EAGAIN for %1 ms at g=%2 "
                                        "(held=%3 inflight=%4)")
                             .arg((int)k_DeadlockMs).arg(g).arg((int)m_Held.size()).arg((int)m_Inflight.size()));
                break;
            }
            sleepPoll();
        }
        if (m_Aborted) {
            m_Status[(size_t)g] = kSendFailed;
            return;
        }

        if (g >= m_Cfg.warmup) {
            m_SendBlock.push_back(blockMs);
            if (m_FirstCountedSendMs < 0) {
                m_FirstCountedSendMs = m_SendMs[(size_t)g];
            }
        }
        if (ret < 0) {
            m_SendErrors++;
            m_Status[(size_t)g] = kSendFailed;
            if (m_FirstSendError.isEmpty()) {
                m_FirstSendError = avErr(ret);
            }
            onError(g, "send");
            return;
        }

        m_Status[(size_t)g] = kSent;
        m_Inflight.push_back(g);
        m_Sent++;
        m_LastSentG = g;
        if (m_FirstSendMs < 0) {
            m_FirstSendMs = m_SendMs[(size_t)g];
        }

        const int outBefore = m_FramesOut;
        drain();
        if (m_FramesOut == outBefore) {
            // 送出後 receive_frame 立刻回 EAGAIN：累積「還沒吐幀就又收了幾個封包」≈ 管線深度
            m_NoOutStreak++;
            m_EagainMax = qMax(m_EagainMax, m_NoOutStreak);
        }
    }

    void drain()
    {
        for (;;) {
            if (m_Spare == nullptr) {
                m_Spare = av_frame_alloc();
                if (m_Spare == nullptr) {
                    abortRun(QStringLiteral("av_frame_alloc failed"));
                    return;
                }
            }
            const int ret = avcodec_receive_frame(m_Dec.ctx, m_Spare);
            if (ret == 0) {
                m_LastRecvError = 0;
                AVFrame* f = m_Spare;
                m_Spare = nullptr;
                onFrame(f);
                if (m_Aborted) {
                    return;
                }
                continue;
            }
            if (ret == AVERROR(EAGAIN)) {
                m_LastRecvError = 0;
                break;
            }
            if (ret == AVERROR_EOF) {
                m_EofSeen = true;
                break;
            }
            // 同一個錯誤在輪詢中連續回報只算一次（recvErrorCalls 記總呼叫數）
            m_RecvErrorCalls++;
            if (ret != m_LastRecvError) {
                m_LastRecvError = ret;
                m_RecvErrors++;
                if (m_FirstRecvError.isEmpty()) {
                    m_FirstRecvError = avErr(ret);
                }
                onError(m_LastSentG, "receive");
            }
            break;
        }
        checkStall();
    }

    // 停頓：decoder 裡還有幀，卻超過門檻（--fps 時 3 個幀間隔，否則 100 ms）沒有輸出。
    // 刻意不因為「一直沒輸出」而中止：送出照常被接受時繼續送（同串流），--loop 的下一輪
    // IDR 才看得到 decoder 能不能自己恢復。真正卡死（send 一直 EAGAIN）在 sendOne() 處理。
    void checkStall()
    {
        if (m_Inflight.empty() || m_InEofDrain || m_InStall) {
            return;
        }
        if (m_Inflight.front() < m_Cfg.warmup) {
            return;
        }
        // 參考點取「最後一次輸出」與「最舊那個在飛封包的送出時間」較晚者：decoder 閒著時
        // 不算停頓
        const double ref = qMax(m_LastOutMs, m_SendMs[(size_t)m_Inflight.front()]);
        if (nowMs() - ref > m_StallThresholdMs) {
            m_InStall = true;
            m_Stalls++;
            m_StallStartMs = ref;
        }
    }

    void onFrame(AVFrame* f)
    {
        const double t = nowMs();
        const int64_t pts = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;

        int g = -1;
        if (pts != AV_NOPTS_VALUE && pts >= 0 && pts < m_Total) {
            const uint8_t st = m_Status[(size_t)pts];
            if (st == kSent) {
                g = (int)pts;
            }
            else if (st == kSkipped) {
                // 先前以為被吃掉了，其實只是晚出來（輸出順序和送出順序不同）
                m_Skipped--;
                m_PtsNonMonotonic++;
                g = (int)pts;
            }
            else if (st == kFlushed) {
                m_LostInFlush--;
                g = (int)pts;
            }
        }
        if (g < 0) {
            // pts 帶不回來（v4l2 timestamp 換算失效等）：退回 FIFO 對應
            m_PtsMismatch++;
            if (!m_Inflight.empty()) {
                g = m_Inflight.front();
            }
        }
        if (g < 0) {
            m_OrphanFrames++;
            av_frame_free(&f);
            return;
        }

        // 串流 bitstream 沒有 B 幀、輸出照送出順序：比 g 早還在飛的幀都當作被 decoder 吃掉
        while (!m_Inflight.empty() && m_Inflight.front() < g) {
            m_Status[(size_t)m_Inflight.front()] = kSkipped;
            m_Skipped++;
            m_Inflight.pop_front();
        }
        if (!m_Inflight.empty() && m_Inflight.front() == g) {
            m_Inflight.pop_front();
        }
        else {
            auto it = std::find(m_Inflight.begin(), m_Inflight.end(), g);
            if (it != m_Inflight.end()) {
                m_Inflight.erase(it);
            }
        }
        if (g <= m_LastOutG) {
            m_PtsNonMonotonic++;
        }
        m_LastOutG = qMax(m_LastOutG, g);

        m_Status[(size_t)g] = kReceived;
        m_OutMs[(size_t)g] = t;
        m_FramesOut++;
        m_NoOutStreak = 0;

        const double lat = t - m_SendMs[(size_t)g];
        if (m_InEofDrain) {
            // EOF 擠出來的幀含等待時間，不列入延遲統計（串流時不會有 EOF）
            m_FramesAtEof++;
        }
        else if (g >= m_Cfg.warmup) {
            m_Latency.push_back(lat);
            m_CountedFrames++;
            m_LastCountedOutMs = t;
        }

        // 錯誤旗標：v4l2 的 V4L2_BUF_FLAG_ERROR 在 cgutman fork 變成
        // FF_DECODE_ERROR_INVALID_BITSTREAM，而且不丟幀
        const bool corruptFlag = (f->flags & AV_FRAME_FLAG_CORRUPT) != 0;
        if (f->decode_error_flags != 0 || corruptFlag) {
            m_ErrFlagFrames++;
            m_ErrFlagsOr |= f->decode_error_flags;
            if (corruptFlag) {
                m_CorruptFlagFrames++;
            }
            m_OutState[(size_t)g] = 1;
        }
        else {
            m_OutState[(size_t)g] = 0;
            if (m_EpisodeOpen && g > m_EpisodeStartG) {
                m_EpisodeOpen = false;
            }
        }

        if (m_LastOutMs >= 0 && g >= m_Cfg.warmup && !m_InEofDrain) {
            m_MaxGapMs = qMax(m_MaxGapMs, t - m_LastOutMs);
        }
        if (m_InStall) {
            m_StallMs += t - m_StallStartMs;
            m_InStall = false;
        }
        m_LastOutMs = t;

        if (m_FramesOut == 1) {
            m_FirstFrameMs = t - m_FirstSendMs;
            m_FirstWidth = f->width;
            m_FirstHeight = f->height;
            m_FirstFrameFormat = QString::fromLatin1(pixFmtName(f->format));
            m_FirstFrame = describeFrame(f, m_DrmSummary);
            m_FirstFrame[QStringLiteral("afterMs")] = round3(m_FirstFrameMs);
        }

        if (m_Cmp != nullptr && m_Cmp->active) {
            m_Psnr[(size_t)g] = (float)m_Cmp->psnrFor(g % m_Cfg.nf, f);
        }
#ifdef HAVE_LIBPLACEBO_VULKAN
        if (m_Mapper != nullptr) {
            m_Mapper->mapTimed(f);
        }
#endif

        if (m_Cfg.verbose) {
            fprintf(stdout, "frame g=%d k=%d lat=%.3f flags=0x%x corrupt=%d psnr=%.2f%s\n",
                    g, g % m_Cfg.nf, lat, (unsigned)f->decode_error_flags, corruptFlag ? 1 : 0,
                    (double)m_Psnr[(size_t)g], m_InEofDrain ? " (eof)" : "");
        }

        // 錯誤區段在記錄完這一幀之後才處理：flush 會清掉之後還在 decoder 裡的幀
        if (m_OutState[(size_t)g] > 0) {
            onError(g, "frame-flags");
        }

        if (m_Cfg.hold > 0) {
            // 延後 unref：模擬 Pacer／GPU 持有（PoC-3c 用來找 capture buffer 下限）
            m_Held.push_back(f);
            while ((int)m_Held.size() > m_Cfg.hold) {
                AVFrame* old = m_Held.front();
                m_Held.pop_front();
                av_frame_free(&old);
            }
        }
        else {
            av_frame_free(&f);
        }
    }

    void tail()
    {
        // 1. 像串流一樣等最後幾幀自然出來（不送 EOF）
        const double waitMs = m_Cfg.fps > 0 ? qMax(3.0 * m_IntervalMs, 50.0) : 200.0;
        const Clock::time_point until = Clock::now() +
            std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(waitMs));
        while (!m_Inflight.empty() && !m_Aborted && Clock::now() < until) {
            drain();
            if (!m_Inflight.empty()) {
                sleepPoll();
            }
        }
        if (m_Aborted || m_Inflight.empty()) {
            return;
        }

        // 2. 送 EOF 把 decoder 內部還壓著的幀擠出來（這些幀不列入延遲統計）
        m_InEofDrain = true;
        const int r = avcodec_send_packet(m_Dec.ctx, nullptr);
        if (r >= 0 || r == AVERROR_EOF) {
            const Clock::time_point eofUntil = Clock::now() + std::chrono::seconds(3);
            while (!m_EofSeen && !m_Aborted && Clock::now() < eofUntil) {
                drain();
                if (!m_EofSeen) {
                    sleepPoll();
                }
            }
        }
        m_InEofDrain = false;
    }

    void progress(int g)
    {
        const Clock::time_point now = Clock::now();
        if (now - m_LastProgress >= std::chrono::seconds(10)) {
            m_LastProgress = now;
            ProbeUtil::printLine(k_Tag, "bench: progress sent=%d/%d out=%d errFlag=%d stall=%d",
                                 g + 1, m_Total, m_FramesOut, m_ErrFlagFrames, m_Stalls);
        }
    }

    void releaseAll()
    {
        for (AVFrame* f : m_Held) {
            av_frame_free(&f);
        }
        m_Held.clear();
        av_frame_free(&m_Spare);
        av_packet_free(&m_Pkt);
    }

    BenchDecoder& m_Dec;
    const QVector<BenchPacket>& m_Pkts;
    RunConfig m_Cfg;
    CompareState* m_Cmp;
#ifdef HAVE_LIBPLACEBO_VULKAN
    PlMapper* m_Mapper = nullptr;
#endif

    AVPacket* m_Pkt = nullptr;
    AVFrame* m_Spare = nullptr;
    QByteArray m_Scratch;
    std::deque<AVFrame*> m_Held;
    std::deque<int> m_Inflight;

    int m_Total = 0;
    double m_IntervalMs = 0;
    double m_StallThresholdMs = 0;
    Clock::time_point m_T0;
    Clock::time_point m_LastProgress;

    std::vector<uint8_t> m_Status;
    std::vector<double> m_SendMs;
    std::vector<double> m_OutMs;
    std::vector<int8_t> m_OutState;   // -1 沒輸出；0 乾淨；1 帶錯誤旗標
    std::vector<float> m_Psnr;        // -1 = 沒有
    QVector<BenchEvent> m_Events;

    std::vector<double> m_Latency;
    std::vector<double> m_SendBlock;

    bool m_Aborted = false;
    QString m_AbortReason;
    bool m_EofSeen = false;
    bool m_InEofDrain = false;
    bool m_EpisodeOpen = false;
    bool m_EpisodeFlushed = false;
    int m_EpisodeStartG = -1;

    int m_Sent = 0;
    int m_Dropped = 0;
    int m_Corrupted = 0;
    int m_FramesOut = 0;
    int m_FramesAtEof = 0;
    int m_CountedFrames = 0;
    int m_SendErrors = 0;
    int m_RecvErrors = 0;
    int m_RecvErrorCalls = 0;
    int m_LastRecvError = 0;
    QString m_FirstSendError;
    QString m_FirstRecvError;
    int m_ErrFlagFrames = 0;
    int m_ErrFlagsOr = 0;
    int m_CorruptFlagFrames = 0;
    int m_Stalls = 0;
    double m_StallMs = 0;
    double m_StallStartMs = 0;
    bool m_InStall = false;
    double m_MaxGapMs = 0;
    int m_EagainMax = 0;
    int m_NoOutStreak = 0;
    int m_SendEagain = 0;
    int m_SendEagainMax = 0;
    int m_PtsMismatch = 0;
    int m_PtsNonMonotonic = 0;
    int m_Skipped = 0;
    int m_NeverOutput = 0;
    int m_OrphanFrames = 0;
    int m_Flushes = 0;
    int m_LostInFlush = 0;
    int m_LastSentG = -1;
    int m_LastOutG = -1;

    double m_FirstSendMs = -1;
    double m_FirstCountedSendMs = -1;
    double m_LastCountedOutMs = -1;
    double m_LastOutMs = -1;
    double m_FirstFrameMs = -1;
    double m_WallMs = 0;

    QJsonObject m_FirstFrame;
    QString m_FirstFrameFormat;
    QString m_DrmSummary = QStringLiteral("-");
    int m_FirstWidth = 0;
    int m_FirstHeight = 0;
};

// ---------------------------------------------------------------------------

QJsonObject inputJson(const InputInfo& in, const QVector<BenchPacket>& pkts)
{
    QJsonObject j;
    j[QStringLiteral("path")] = in.path;
    j[QStringLiteral("codec")] = in.codec;
    j[QStringLiteral("container")] = in.container;
    j[QStringLiteral("fileBytes")] = in.fileBytes;
    j[QStringLiteral("accessUnits")] = (int)pkts.size();
    j[QStringLiteral("keyframes")] = in.keyframes;
    j[QStringLiteral("width")] = in.width;
    j[QStringLiteral("height")] = in.height;
    j[QStringLiteral("truncatedTail")] = in.truncatedTail;
    if (in.container == QLatin1String("ivf")) {
        j[QStringLiteral("ivfDeclaredFrames")] = in.ivfDeclaredFrames;
        j[QStringLiteral("ivfEmptyFrames")] = in.ivfEmptyFrames;
    }
    qint64 maxAu = 0;
    for (const BenchPacket& p : pkts) {
        maxAu = qMax<qint64>(maxAu, p.size);
    }
    j[QStringLiteral("maxAuBytes")] = maxAu;
    return j;
}

QJsonObject decoderJson(const BenchDecoder& d, const QJsonArray& tried)
{
    QJsonObject j;
    j[QStringLiteral("name")] = QString::fromUtf8(d.cand.codec->name);
    j[QStringLiteral("label")] = d.cand.label;
    j[QStringLiteral("kind")] = QString::fromLatin1(kindName(d.cand.kind));
    if (d.cand.hwCfg != nullptr) {
        j[QStringLiteral("hwDevice")] = QString::fromLatin1(av_hwdevice_get_type_name(d.cand.hwCfg->device_type));
        j[QStringLiteral("hwPixFmt")] = QString::fromLatin1(pixFmtName(d.cand.hwCfg->pix_fmt));
    }
    j[QStringLiteral("threadType")] = d.threadType;
    j[QStringLiteral("threadCount")] = d.threadCount;
    if (d.cand.kind == DecKind::V4l2m2m) {
        j[QStringLiteral("numOutputBuffers")] = d.outputBuffers;
        j[QStringLiteral("numCaptureBuffers")] = d.captureBuffers;
    }
    if (!d.pixFmtHint.isEmpty()) {
        j[QStringLiteral("pixFmtHint")] = d.pixFmtHint;
    }
    if (!d.unusedOptions.isEmpty()) {
        j[QStringLiteral("unusedOptions")] = QJsonArray::fromStringList(d.unusedOptions);
    }
    j[QStringLiteral("openMs")] = round3(d.openMs);
    j[QStringLiteral("getFormatCalls")] = d.gf.calls;
    j[QStringLiteral("getFormatOffered")] = QJsonArray::fromStringList(d.gf.firstOffer);
    j[QStringLiteral("getFormatChosen")] = QString::fromLatin1(pixFmtName(d.gf.chosen));
    j[QStringLiteral("drmPrimeOffered")] = d.gf.drmPrimeOffered;

    QJsonArray supported;
    const AVPixelFormat* fmts = codecPixFmts(d.cand.codec);
    for (int i = 0; fmts != nullptr && fmts[i] != AV_PIX_FMT_NONE; i++) {
        supported.append(QString::fromLatin1(pixFmtName(fmts[i])));
    }
    j[QStringLiteral("codecPixFmts")] = supported;
    j[QStringLiteral("tried")] = tried;
    return j;
}

int benchMain(const DecodeBenchOptions& o, QJsonObject& root, QJsonArray& notes)
{
    auto fail = [&](int rc, const QString& msg) {
        ProbeUtil::printLine(k_Tag, "bench: error: %s", qUtf8Printable(msg));
        root[QStringLiteral("error")] = msg;
        return rc;
    };

    if (o.verbose) {
        av_log_set_level(AV_LOG_DEBUG);
    }

    // ---- 參數 ----
    QString out = o.out.trimmed().toLower();
    if (out == QLatin1String("drm")) {
        out = QStringLiteral("drm_prime");
    }
    if (out != QLatin1String("drm_prime") && out != QLatin1String("sw")) {
        return fail(ProbeUtil::kExitBadInput, QStringLiteral("--out must be drm_prime or sw"));
    }
    if (o.fps < 0 || o.fps > 1000 || o.frames < 0 || o.loop < 1 || o.warmup < 0 || o.hold < 0 ||
            o.dropEvery < 0 || o.captureBuffers < -1 || o.outputBuffers < -1) {
        return fail(ProbeUtil::kExitBadInput,
                    QStringLiteral("invalid numeric option (fps 0..1000, frames>=0, loop>=1, warmup>=0, hold>=0, drop-every>=0)"));
    }

    // ---- 輸入 ----
    InputInfo in;
    QVector<BenchPacket> pkts;
    QString err;
    int rc = loadInput(o, in, pkts, err);
    if (rc != ProbeUtil::kExitOk) {
        return fail(rc, err);
    }
    root[QStringLiteral("input")] = inputJson(in, pkts);
    ProbeUtil::printLine(k_Tag, "bench: input file=%s codec=%s container=%s aus=%d keyframes=%d %dx%d bytes=%lld%s",
                         qUtf8Printable(QFileInfo(in.path).fileName()), qUtf8Printable(in.codec),
                         qUtf8Printable(in.container), (int)pkts.size(), in.keyframes, in.width, in.height,
                         (long long)in.fileBytes, in.truncatedTail ? " truncatedTail=1" : "");
    if (in.keyframes == 0) {
        notes.append(QStringLiteral("input has no detected keyframe; decoders may output nothing"));
    }
    if (in.width <= 0 || in.height <= 0) {
        notes.append(QStringLiteral("stream dimensions unknown before decoding; AVCodecContext width/height left at 0"));
    }

    const int nf = o.frames > 0 ? qMin(o.frames, (int)pkts.size()) : (int)pkts.size();
    if (o.frames > (int)pkts.size()) {
        notes.append(QStringLiteral("--frames %1 clamped to the %2 access units in the file").arg(o.frames).arg((int)pkts.size()));
    }
    // 總幀數在開 decoder、建參考之前先擋：int 相乘會溢位，太大則 per-frame 表配置失敗（bad_alloc 或
    // 被 OOM killer 收掉），兩種都拿不到 JSON 與結束碼
    const qint64 scheduled = (qint64)nf * o.loop;
    if (scheduled > k_MaxScheduledFrames) {
        return fail(ProbeUtil::kExitBadInput,
                    QStringLiteral("%1 access units x --loop %2 = %3 frames exceeds the limit of %4; lower --loop or --frames")
                        .arg(nf).arg(o.loop).arg(scheduled).arg(k_MaxScheduledFrames));
    }

    // ---- 候選 ----
    QVector<BenchCandidate> cands;
    bool autoMode = false;
    rc = buildCandidates(o, in, cands, autoMode, err);
    if (rc != ProbeUtil::kExitOk) {
        return fail(rc, err);
    }
    if ((o.captureBuffers >= 0 || o.outputBuffers >= 0) &&
            std::none_of(cands.begin(), cands.end(), [](const BenchCandidate& c) { return c.kind == DecKind::V4l2m2m; })) {
        notes.append(QStringLiteral("--capture-buffers/--output-buffers only apply to *_v4l2m2m decoders"));
    }

    // ---- --compare-sw 參考（在開計時用的 decoder 之前做完，讓受測 decoder 開好就開跑）----
    CompareState cmp;
    QJsonObject cmpJson;
    if (o.compareSw) {
        QString why;
        if (buildReference(cmp, pkts, nf, in, why)) {
            cmp.active = true;
            ProbeUtil::printLine(k_Tag, "bench: compare-sw reference=%s frames=%d grid=%dx%d step=%d",
                                 qUtf8Printable(cmp.refDecoder), cmp.refFrames, cmp.grid.gw, cmp.grid.gh, cmp.grid.step);
            notes.append(QStringLiteral("compare-sw samples luma of every frame inside the timed loop; latency/throughput are perturbed"));
        }
        else {
            cmp.error = why;
            ProbeUtil::printLine(k_Tag, "bench: compare-sw unavailable (%s)", qUtf8Printable(why));
        }
    }

    // ---- 選 decoder ----
    OpenParams op;
    op.outDrm = out == QLatin1String("drm_prime");
    op.captureBuffers = o.captureBuffers;
    op.outputBuffers = o.outputBuffers;
    op.width = in.width;
    op.height = in.height;

    QJsonArray tried;
    std::unique_ptr<BenchDecoder> dec;
    for (const BenchCandidate& c : cands) {
        auto d = std::make_unique<BenchDecoder>();
        QString why;
        bool ok = openDecoder(*d, c, op, why);
        if (ok && autoMode) {
            ok = trialDecode(*d, pkts, why);
            if (ok) {
                // 試解過的 instance 狀態已經動過：關掉，重開一個乾淨的給計時用
                d = std::make_unique<BenchDecoder>();
                ok = openDecoder(*d, c, op, why);
            }
        }
        QJsonObject t;
        t[QStringLiteral("decoder")] = c.label;
        t[QStringLiteral("kind")] = QString::fromLatin1(kindName(c.kind));
        t[QStringLiteral("result")] = ok ? QStringLiteral("ok") : why;
        tried.append(t);
        if (autoMode || !ok) {
            ProbeUtil::printLine(k_Tag, "bench: candidate %s (%s): %s",
                                 qUtf8Printable(c.label), kindName(c.kind), ok ? "ok" : qUtf8Printable(why));
        }
        if (ok) {
            dec = std::move(d);
            break;
        }
    }
    if (!dec) {
        root[QStringLiteral("decoder")] = QJsonObject{{QStringLiteral("tried"), tried}};
        return fail(ProbeUtil::kExitCapabilityAbsent, QStringLiteral("no usable decoder for this input"));
    }
    ProbeUtil::printLine(k_Tag, "bench: decoder=%s kind=%s threads=%d open=%.1fms%s",
                         qUtf8Printable(dec->cand.label), kindName(dec->cand.kind), dec->threadCount, dec->openMs,
                         dec->cand.kind == DecKind::V4l2m2m
                             ? qUtf8Printable(QStringLiteral(" outBufs=%1 capBufs=%2").arg(dec->outputBuffers).arg(dec->captureBuffers))
                             : "");

    // ---- --map-vulkan ----
    QJsonObject mapJson;
#ifdef HAVE_LIBPLACEBO_VULKAN
    std::unique_ptr<PlMapper> mapper;
    if (o.mapVulkan) {
        mapper = std::make_unique<PlMapper>();
        QString why;
        if (mapper->init(o.verbose, why)) {
            ProbeUtil::printLine(k_Tag, "bench: map-vulkan device=\"%s\"", qUtf8Printable(mapper->deviceName()));
            notes.append(QStringLiteral("map-vulkan maps every frame inside the timed loop; latency/throughput are perturbed"));
        }
        else {
            ProbeUtil::printLine(k_Tag, "bench: map-vulkan unavailable (%s)", qUtf8Printable(why));
            mapJson[QStringLiteral("available")] = false;
            mapJson[QStringLiteral("error")] = why;
            mapper.reset();
        }
    }
#else
    if (o.mapVulkan) {
        ProbeUtil::printLine(k_Tag, "bench: map-vulkan unavailable (built without libplacebo Vulkan)");
        mapJson[QStringLiteral("available")] = false;
        mapJson[QStringLiteral("error")] = QStringLiteral("built without HAVE_LIBPLACEBO_VULKAN");
    }
#endif

    // ---- 計時 ----
    RunConfig cfg;
    cfg.fps = o.fps;
    cfg.nf = nf;
    cfg.loops = o.loop;
    cfg.warmup = o.warmup;
    cfg.hold = o.hold;
    cfg.drop = QSet<int>(o.dropFrames.begin(), o.dropFrames.end());
    cfg.dropEvery = o.dropEvery;
    cfg.corrupt = QSet<int>(o.corruptFrames.begin(), o.corruptFrames.end());
    cfg.flushOnError = o.flushOnError;
    cfg.verbose = o.verbose;
    if (cfg.drop.contains(0) || cfg.corrupt.contains(0)) {
        notes.append(QStringLiteral("frame 0 (the leading keyframe) is dropped/corrupted; the decoder may never produce output"));
    }

    BenchRun run(*dec, pkts, cfg, cmp.active ? &cmp : nullptr);
#ifdef HAVE_LIBPLACEBO_VULKAN
    run.setMapper(mapper.get());
#endif
    run.execute();

    // ---- 結果 ----
    const Dist lat = run.latency();
    const int w = run.firstWidth() > 0 ? run.firstWidth() : in.width;
    const int h = run.firstHeight() > 0 ? run.firstHeight() : in.height;
    const QString outFmt = run.firstFrameFormat().isEmpty() ? QStringLiteral("-") : run.firstFrameFormat();

    if (!run.firstFrame().isEmpty()) {
        const QJsonObject ff = run.firstFrame();
        ProbeUtil::printLine(k_Tag, "bench: first-frame %dx%d fmt=%s sw=%s drm=%s afterMs=%.2f",
                             w, h, qUtf8Printable(outFmt),
                             qUtf8Printable(ff.value(QStringLiteral("swFormat")).toString(QStringLiteral("-"))),
                             qUtf8Printable(run.drmSummary()),
                             ff.value(QStringLiteral("afterMs")).toDouble());
    }
    ProbeUtil::printLine(k_Tag,
                         "bench: decoder=%s out=%s drm=%s %dx%d fps=%d n=%d lat p50=%.2f p95=%.2f p99=%.2f max=%.2f "
                         "errFlag=%d stall=%d eagainMax=%d",
                         qUtf8Printable(dec->cand.label), qUtf8Printable(outFmt), qUtf8Printable(run.drmSummary()),
                         w, h, o.fps, lat.n, lat.p50, lat.p95, lat.p99, lat.maxv,
                         run.errFlagFrames(), run.stalls(), run.eagainMax());

    const QJsonObject counters = run.countersJson();
    const QJsonObject timing = run.timingJson();
    const QJsonObject sb = timing.value(QStringLiteral("sendBlockMs")).toObject();
    ProbeUtil::printLine(k_Tag,
                         "bench: sendBlock p50=%.3f p95=%.3f max=%.3f sendEagain=%d sendErr=%d recvErr=%d "
                         "ptsMismatch=%d skipped=%d flushes=%d throughput=%.1ffps wall=%.0fms",
                         sb.value(QStringLiteral("p50")).toDouble(), sb.value(QStringLiteral("p95")).toDouble(),
                         sb.value(QStringLiteral("max")).toDouble(),
                         counters.value(QStringLiteral("sendEagain")).toInt(),
                         counters.value(QStringLiteral("sendErrors")).toInt(),
                         counters.value(QStringLiteral("recvErrors")).toInt(),
                         counters.value(QStringLiteral("ptsMismatch")).toInt(),
                         counters.value(QStringLiteral("skipped")).toInt(),
                         counters.value(QStringLiteral("flushes")).toInt(),
                         run.throughputFps(), timing.value(QStringLiteral("wallMs")).toDouble());

    QJsonObject eventSummary;
    QStringList eventLines;
    const QJsonArray events = run.analyzeEvents(eventSummary, eventLines);
    // eventLines 已含「... N more event(s)」那行（被併進注入事件的 error 事件不單獨成行，由 analyzeEvents 自己算）
    for (const QString& line : eventLines) {
        ProbeUtil::printLine(k_Tag, "%s", qUtf8Printable(line));
    }
    if (run.eventCount() > 0 && cmp.active) {
        // 新欄位接在最後，既有欄位的順序不變
        ProbeUtil::printLine(k_Tag, "bench: events=%d selfHealed=%d needIdr=%d healMax=%d healMean=%.1f inconclusive=%d merged=%d",
                             run.eventCount(),
                             eventSummary.value(QStringLiteral("selfHealed")).toInt(),
                             eventSummary.value(QStringLiteral("needIdr")).toInt(),
                             eventSummary.value(QStringLiteral("healMax")).toInt(),
                             eventSummary.value(QStringLiteral("healMean")).toDouble(),
                             eventSummary.value(QStringLiteral("inconclusive")).toInt(),
                             eventSummary.value(QStringLiteral("merged")).toInt());
    }

    if (o.compareSw) {
        cmpJson[QStringLiteral("available")] = cmp.active;
        if (cmp.active) {
            const QJsonObject ps = run.psnrSummary();
            cmpJson[QStringLiteral("reference")] = cmp.refDecoder;
            cmpJson[QStringLiteral("referenceFrames")] = cmp.refFrames;
            cmpJson[QStringLiteral("grid")] = QJsonObject{{QStringLiteral("width"), cmp.grid.gw},
                                                          {QStringLiteral("height"), cmp.grid.gh},
                                                          {QStringLiteral("step"), cmp.grid.step}};
            cmpJson[QStringLiteral("psnr")] = ps;
            cmpJson[QStringLiteral("healThresholdDb")] = k_HealPsnrDb;
            cmpJson[QStringLiteral("sampleFailures")] = cmp.failures;
            if (!cmp.firstFailure.isEmpty()) {
                cmpJson[QStringLiteral("firstFailure")] = cmp.firstFailure;
            }
            ProbeUtil::printLine(k_Tag, "bench: compare-sw ref=%s compared=%d psnr min=%.2f mean=%.2f below40=%d failures=%d%s%s",
                                 qUtf8Printable(cmp.refDecoder), ps.value(QStringLiteral("frames")).toInt(),
                                 ps.value(QStringLiteral("min")).toDouble(-1), ps.value(QStringLiteral("mean")).toDouble(-1),
                                 ps.value(QStringLiteral("below40")).toInt(), cmp.failures,
                                 cmp.firstFailure.isEmpty() ? "" : " firstFailure=",
                                 qUtf8Printable(cmp.firstFailure));
        }
        else {
            cmpJson[QStringLiteral("error")] = cmp.error;
        }
    }

#ifdef HAVE_LIBPLACEBO_VULKAN
    if (mapper) {
        mapJson = mapper->json();
        const Dist md = mapper->mapDist();
        ProbeUtil::printLine(k_Tag, "bench: map-vulkan device=\"%s\" mapped=%d map p50=%.3f p95=%.3f max=%.3f fail=%d unsupported=%d",
                             qUtf8Printable(mapper->deviceName()), md.n, md.p50, md.p95, md.maxv,
                             mapper->failures(), mapper->unsupported());
    }
#endif

    if (run.aborted()) {
        ProbeUtil::printLine(k_Tag, "bench: ABORTED %s", qUtf8Printable(run.abortReason()));
    }

    root[QStringLiteral("decoder")] = decoderJson(*dec, tried);
    root[QStringLiteral("firstFrame")] = run.firstFrame();
    root[QStringLiteral("timing")] = timing;
    root[QStringLiteral("counters")] = counters;
    root[QStringLiteral("events")] = events;
    root[QStringLiteral("eventSummary")] = eventSummary;
    if (o.compareSw) {
        root[QStringLiteral("compareSw")] = cmpJson;
    }
    if (o.mapVulkan) {
        root[QStringLiteral("mapVulkan")] = mapJson;
    }
    if (run.aborted()) {
        root[QStringLiteral("aborted")] = run.abortReason();
    }

    if (run.aborted()) {
        return ProbeUtil::kExitRuntimeError;
    }
    if (run.framesOut() == 0) {
        // 指定的 decoder 開得起來卻一幀都解不出 → 視同「開不起來」；auto 已經試解過，
        // 走到這裡是執行中才壞掉
        root[QStringLiteral("error")] = QStringLiteral("decoder produced no frames");
        return autoMode ? ProbeUtil::kExitRuntimeError : ProbeUtil::kExitCapabilityAbsent;
    }
    return ProbeUtil::kExitOk;
}

} // namespace

int runDecodeBench(const DecodeBenchOptions& options)
{
    ProbeUtil::beginProbe("decode-bench");
    const QString jsonPath = options.jsonPath.isEmpty()
            ? ProbeUtil::defaultJsonPath(QStringLiteral("decode-bench"))
            : options.jsonPath;

    // Windows：SDL 初始化任何子系統時會把系統計時器調到 1 ms，輪詢的 sleep 才不會變成 15.6 ms
    const bool timerInit = SDL_InitSubSystem(SDL_INIT_TIMER) == 0;

    QJsonObject root;
    root[QStringLiteral("probe")] = QStringLiteral("decode-bench");   // 根鍵與 xr-probe／v4l2-probe 一致
    root[QStringLiteral("schema")] = 1;
    root[QStringLiteral("options")] = optionsJson(options);
    QJsonArray notes;

    int rc = benchMain(options, root, notes);

    if (timerInit) {
        SDL_QuitSubSystem(SDL_INIT_TIMER);
    }

    root[QStringLiteral("notes")] = notes;
    root[QStringLiteral("rc")] = rc;
    root[QStringLiteral("env")] = SfEnv::snapshot();

    const bool written = ProbeUtil::writeJson(jsonPath, root);
    if (!written && rc == ProbeUtil::kExitOk) {
        rc = ProbeUtil::kExitJsonWriteFailed;
    }
    return ProbeUtil::finish(rc, written ? jsonPath : QString());
}

#endif // HAVE_FFMPEG

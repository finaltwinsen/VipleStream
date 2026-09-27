// VipleStream §SF-PROBE（M2a）— V4L2 能力列舉的實作（介面與使用者見 v4l2caps.h）。
//
// 依 kernel 的 stateful decoder 規格（userspace-api/media/v4l/dev-decoder.rst）：
//   - OUTPUT 的 ENUM_FMT 列出全部 coded 格式；CAPTURE 的 ENUM_FMT 只列「目前 OUTPUT 格式」
//     能輸出的 raw 格式，profile／level 的 menu 也以目前 OUTPUT 格式為準 → 每個 coded 格式
//     都先 S_FMT OUTPUT，再列 CAPTURE 格式與控制項。控制項逐格式整份走一遍：Qualcomm 下游
//     driver 會在換 codec 時重建控制項，只走一次會漏。
//   - MIN_BUFFERS_FOR_CAPTURE 要等 SOURCE_CHANGE（header 解析完）之後才準；S_FMT 之後讀到的
//     只當「靜態」值，header-test 另外讀串流後的值。
//   - drain（DEC_CMD_STOP）只有兩個 queue 都在串流時才會啟動（之前送是 no-op），所以
//     header-test 只在 CAPTURE 已串流、而且等不到輸出時才送。
// 單平面（V4L2_CAP_VIDEO_M2M）與多平面（M2M_MPLANE）都支援：builder 上的 vicodec 可能以
// multiplanar=1 載入，iris 是 MPLANE。兩種都有時照 FFmpeg v4l2_m2m.c 優先用 MPLANE。
//
// 規則：只看 capability 旗標，不寫死 driver 名稱；所有 ioctl 遇 EINTR 重試；每段 poll 都
// 有 1 s 上限；header-test 的每條離開路徑都 STREAMOFF → munmap → REQBUFS(0) → close（RAII）。

#include "v4l2caps.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX) && defined(__has_include)
#if __has_include(<linux/videodev2.h>)
#define VIPLE_HAVE_V4L2 1
#endif
#endif

#ifdef VIPLE_HAVE_V4L2

#include "cli/probeutil.h"

#ifdef HAVE_FFMPEG
// header-test 的測試幀：app 啟動時測 decoder 用的同一組 720p 幀（ffmpeg_videosamples.cpp）
#include "streaming/video/ffmpeg.h"
#endif

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/videodev2.h>

#if __has_include(<sys/xattr.h>)
#include <sys/xattr.h>
#define VIPLE_HAVE_XATTR 1
#endif

namespace {

const char* const kTag = "VIPLE-V4L2-PROBE";

constexpr int kWaitMs = 1000;                          // 每一段 poll 等待的上限
constexpr uint32_t kCodedSizeImage = 2 * 1024 * 1024;  // S_FMT OUTPUT 要求的 bitstream buffer 大小（driver 可調整）
constexpr uint32_t kEnumWidth = 1920;                  // 列舉 CAPTURE 格式時給的 placeholder 解析度
constexpr uint32_t kEnumHeight = 1080;
constexpr int kMaxEnum = 64;                           // ENUM_* 迴圈上限（防 driver bug 造成無窮迴圈）
constexpr int kMaxControls = 512;
constexpr int kMaxMenuItems = 64;
constexpr int kMaxEvents = 16;
constexpr int kMaxCaptureBuffers = 32;
constexpr int kOutputBuffers = 2;

// 測試幀固定 720p（ffmpeg_videosamples.cpp 的註解）
constexpr uint32_t kTestFrameWidth = 1280;
constexpr uint32_t kTestFrameHeight = 720;

using Clock = std::chrono::steady_clock;

constexpr uint32_t fcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

// ── 控制項 ID ──
// 直接寫數值（ABI 固定）：舊 header 缺巨集時照樣能編；有巨集時用 static_assert 對帳。
constexpr uint32_t kCidBase = 0x00980900;       // V4L2_CID_BASE
constexpr uint32_t kCidCodecBase = 0x00990900;  // V4L2_CID_CODEC_BASE（舊名 V4L2_CID_MPEG_BASE）
constexpr uint32_t kCidMinBuffersForCapture = kCidBase + 39;
constexpr uint32_t kCidMinBuffersForOutput = kCidBase + 40;
constexpr uint32_t kCidH264Level = kCidCodecBase + 359;
constexpr uint32_t kCidH264Profile = kCidCodecBase + 363;
constexpr uint32_t kCidVp8Profile = kCidCodecBase + 511;
constexpr uint32_t kCidVp9Profile = kCidCodecBase + 512;
constexpr uint32_t kCidVp9Level = kCidCodecBase + 513;
constexpr uint32_t kCidHevcProfile = kCidCodecBase + 615;
constexpr uint32_t kCidHevcLevel = kCidCodecBase + 616;
constexpr uint32_t kCidHevcTier = kCidCodecBase + 618;
constexpr uint32_t kCidAv1Profile = kCidCodecBase + 655;
constexpr uint32_t kCidAv1Level = kCidCodecBase + 656;

#ifdef V4L2_CID_MIN_BUFFERS_FOR_CAPTURE
static_assert(kCidMinBuffersForCapture == V4L2_CID_MIN_BUFFERS_FOR_CAPTURE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MIN_BUFFERS_FOR_OUTPUT
static_assert(kCidMinBuffersForOutput == V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_H264_LEVEL
static_assert(kCidH264Level == V4L2_CID_MPEG_VIDEO_H264_LEVEL, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_H264_PROFILE
static_assert(kCidH264Profile == V4L2_CID_MPEG_VIDEO_H264_PROFILE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_VP8_PROFILE
static_assert(kCidVp8Profile == V4L2_CID_MPEG_VIDEO_VP8_PROFILE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_VP9_PROFILE
static_assert(kCidVp9Profile == V4L2_CID_MPEG_VIDEO_VP9_PROFILE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_VP9_LEVEL
static_assert(kCidVp9Level == V4L2_CID_MPEG_VIDEO_VP9_LEVEL, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_HEVC_PROFILE
static_assert(kCidHevcProfile == V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_HEVC_LEVEL
static_assert(kCidHevcLevel == V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_HEVC_TIER
static_assert(kCidHevcTier == V4L2_CID_MPEG_VIDEO_HEVC_TIER, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_AV1_PROFILE
static_assert(kCidAv1Profile == V4L2_CID_MPEG_VIDEO_AV1_PROFILE, "V4L2 CID mismatch");
#endif
#ifdef V4L2_CID_MPEG_VIDEO_AV1_LEVEL
static_assert(kCidAv1Level == V4L2_CID_MPEG_VIDEO_AV1_LEVEL, "V4L2 CID mismatch");
#endif

// level menu 的 index 就是 v4l2-controls.h 的 level 列舉值，數值越大 level 越高。
constexpr int kH264Level52 = 16;  // V4L2_MPEG_VIDEO_H264_LEVEL_5_2
constexpr int kH264Level60 = 17;  // V4L2_MPEG_VIDEO_H264_LEVEL_6_0
constexpr int kHevcLevel51 = 8;   // V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1
constexpr int kHevcLevel52 = 9;   // V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2

// 控制項型別（enum v4l2_ctrl_type 的 ABI 值；用數值比對，避開 enum／unsigned 比較警告）
constexpr uint32_t kCtrlInteger = 1;
constexpr uint32_t kCtrlBoolean = 2;
constexpr uint32_t kCtrlMenu = 3;
constexpr uint32_t kCtrlButton = 4;
constexpr uint32_t kCtrlInteger64 = 5;
constexpr uint32_t kCtrlClass = 6;
constexpr uint32_t kCtrlString = 7;
constexpr uint32_t kCtrlBitmask = 8;
constexpr uint32_t kCtrlIntegerMenu = 9;

// ── coded 格式表 ──
#ifdef V4L2_PIX_FMT_AV1
constexpr uint32_t kFourccAv1Stateful = V4L2_PIX_FMT_AV1;
#else
// stateful AV1 的 fourcc 只在較新的 kernel header 才有（iris vpu3x），這個值 UNVERIFIED；
// 認不出來的格式仍以原始 fourcc 與 driver 的 description 記錄，不影響 decoder 判定。
constexpr uint32_t kFourccAv1Stateful = fcc('A', 'V', '0', '1');
#endif

struct CodecInfo {
    uint32_t fourcc;
    const char* name;           // 給人看的 codec 名
    bool stateless;             // Request API 格式：FFmpeg 走 v4l2-request，不是 v4l2m2m
    uint32_t profileCid;        // 0 = 沒有
    uint32_t levelCid;
    uint32_t tierCid;
    const char* headerTestKey;  // 有 app 測試幀可做 header-test 的 codec（"h264"、"hevc"）
};

const CodecInfo kCodecs[] = {
    {fcc('H', '2', '6', '4'), "H264", false, kCidH264Profile, kCidH264Level, 0, "h264"},
    {fcc('A', 'V', 'C', '1'), "H264-no-startcode", false, kCidH264Profile, kCidH264Level, 0, nullptr},
    {fcc('M', '2', '6', '4'), "H264-MVC", false, kCidH264Profile, kCidH264Level, 0, nullptr},
    {fcc('S', '2', '6', '4'), "H264-slice", true, kCidH264Profile, kCidH264Level, 0, nullptr},
    {fcc('H', 'E', 'V', 'C'), "HEVC", false, kCidHevcProfile, kCidHevcLevel, kCidHevcTier, "hevc"},
    {fcc('S', '2', '6', '5'), "HEVC-slice", true, kCidHevcProfile, kCidHevcLevel, kCidHevcTier, nullptr},
    {kFourccAv1Stateful, "AV1", false, kCidAv1Profile, kCidAv1Level, 0, nullptr},
    {fcc('A', 'V', '1', 'F'), "AV1-frame", true, kCidAv1Profile, kCidAv1Level, 0, nullptr},
    {fcc('V', 'P', '9', '0'), "VP9", false, kCidVp9Profile, kCidVp9Level, 0, nullptr},
    {fcc('V', 'P', '9', 'F'), "VP9-frame", true, kCidVp9Profile, kCidVp9Level, 0, nullptr},
    {fcc('V', 'P', '8', '0'), "VP8", false, kCidVp8Profile, 0, 0, nullptr},
    {fcc('V', 'P', '8', 'F'), "VP8-frame", true, kCidVp8Profile, 0, 0, nullptr},
    {fcc('M', 'G', '2', 'S'), "MPEG2-slice", true, 0, 0, 0, nullptr},
    {fcc('F', 'W', 'H', 'T'), "FWHT", false, 0, 0, 0, nullptr},
    {fcc('S', 'F', 'W', 'H'), "FWHT-stateless", true, 0, 0, 0, nullptr},
};

const CodecInfo* findCodec(uint32_t fourcc)
{
    for (const CodecInfo& c : kCodecs) {
        if (c.fourcc == fourcc) {
            return &c;
        }
    }
    return nullptr;
}

const CodecInfo* findCodecByHeaderKey(const QString& key)
{
    for (const CodecInfo& c : kCodecs) {
        if (c.headerTestKey != nullptr && key == QLatin1String(c.headerTestKey)) {
            return &c;
        }
    }
    return nullptr;
}

// OUTPUT 上帶 COMPRESSED 旗標、卻不是視訊 codec 的格式（kernel 的 v4l_fill_fmtdesc 會替 JPEG 這類
// fourcc 自動加上 COMPRESSED）：JPEG／MJPEG 影像 decoder（mtk-jpeg、mxc-jpeg、s5p-jpeg 等）、DV、
// MPEG 多工容器。這些節點對 app 沒用（FFmpeg 也沒有 mjpeg_v4l2m2m），不算 decoder，另記在
// otherCodedFormats。其餘表外格式（例如上面沒查證過的 AV1 fourcc）照舊算 decoder，kind 標 unknown。
const uint32_t kNonVideoCoded[] = {
    fcc('J', 'P', 'E', 'G'),  // V4L2_PIX_FMT_JPEG
    fcc('M', 'J', 'P', 'G'),  // V4L2_PIX_FMT_MJPEG
    fcc('P', 'J', 'P', 'G'),  // V4L2_PIX_FMT_PJPG（Pixart JPEG）
    fcc('J', 'P', 'G', 'L'),  // V4L2_PIX_FMT_JPGL（JPEG-Lite）
    fcc('d', 'v', 's', 'd'),  // V4L2_PIX_FMT_DV
    fcc('M', 'P', 'E', 'G'),  // V4L2_PIX_FMT_MPEG：MPEG-1/2/4 多工容器（TS/PS），不是視訊 elementary stream
};

bool isNonVideoCoded(uint32_t fourcc)
{
    return std::find(std::begin(kNonVideoCoded), std::end(kNonVideoCoded), fourcc) != std::end(kNonVideoCoded);
}

// app 的 v4l2m2m 路徑（FFmpeg 的 <codec>_v4l2m2m）能直接用的 stateful 視訊 codec。FWHT（vicodec）
// 與 stateless 格式仍算 decoder（結束碼不變），但不在這裡：G-α／PoC-0 判讀要看這一項。
bool isStatefulVideoCodec(const CodecInfo& c)
{
    return !c.stateless && (c.fourcc == fcc('H', '2', '6', '4') || c.fourcc == fcc('H', 'E', 'V', 'C') ||
                            c.fourcc == kFourccAv1Stateful || c.fourcc == fcc('V', 'P', '9', '0'));
}

// ── 旗標名稱表（數值來自 videodev2.h，ABI 固定） ──
struct FlagName {
    uint32_t bit;
    const char* name;
};

const FlagName kCapNames[] = {
    {0x00000001, "VIDEO_CAPTURE"},
    {0x00000002, "VIDEO_OUTPUT"},
    {0x00000004, "VIDEO_OVERLAY"},
    {0x00000010, "VBI_CAPTURE"},
    {0x00000020, "VBI_OUTPUT"},
    {0x00000200, "VIDEO_OUTPUT_OVERLAY"},
    {0x00001000, "VIDEO_CAPTURE_MPLANE"},
    {0x00002000, "VIDEO_OUTPUT_MPLANE"},
    {0x00004000, "VIDEO_M2M_MPLANE"},
    {0x00008000, "VIDEO_M2M"},
    {0x00010000, "TUNER"},
    {0x00020000, "AUDIO"},
    {0x00040000, "RADIO"},
    {0x00100000, "SDR_CAPTURE"},
    {0x00200000, "EXT_PIX_FORMAT"},
    {0x00400000, "SDR_OUTPUT"},
    {0x00800000, "META_CAPTURE"},
    {0x01000000, "READWRITE"},
    {0x02000000, "EDID"},
    {0x04000000, "STREAMING"},
    {0x08000000, "META_OUTPUT"},
    {0x10000000, "TOUCH"},
    {0x20000000, "IO_MC"},
    {0x80000000, "DEVICE_CAPS"},
};

const FlagName kFmtFlagNames[] = {
    {0x0001, "COMPRESSED"},
    {0x0002, "EMULATED"},
    {0x0004, "CONTINUOUS_BYTESTREAM"},
    {0x0008, "DYN_RESOLUTION"},
    {0x0010, "ENC_CAP_FRAME_INTERVAL"},
    {0x0020, "CSC_COLORSPACE"},
    {0x0040, "CSC_XFER_FUNC"},
    {0x0080, "CSC_YCBCR_ENC"},
    {0x0100, "CSC_QUANTIZATION"},
    {0x0200, "META_LINE_BASED"},
};

const FlagName kBufCapNames[] = {
    {1u << 0, "MMAP"},
    {1u << 1, "USERPTR"},
    {1u << 2, "DMABUF"},
    {1u << 3, "REQUESTS"},
    {1u << 4, "ORPHANED_BUFS"},
    {1u << 5, "M2M_HOLD_CAPTURE_BUF"},
    {1u << 6, "MMAP_CACHE_HINTS"},
    {1u << 7, "MAX_NUM_BUFFERS"},
    {1u << 8, "REMOVE_BUFS"},
};

const FlagName kBufFlagNames[] = {
    {0x00000001, "MAPPED"},
    {0x00000002, "QUEUED"},
    {0x00000004, "DONE"},
    {0x00000008, "KEYFRAME"},
    {0x00000010, "PFRAME"},
    {0x00000020, "BFRAME"},
    {0x00000040, "ERROR"},
    {0x00000080, "IN_REQUEST"},
    {0x00000100, "TIMECODE"},
    {0x00000200, "M2M_HOLD_CAPTURE_BUF"},
    {0x00000400, "PREPARED"},
    {0x00000800, "NO_CACHE_INVALIDATE"},
    {0x00001000, "NO_CACHE_CLEAN"},
    {0x00002000, "TIMESTAMP_MONOTONIC"},
    {0x00004000, "TIMESTAMP_COPY"},
    {0x00010000, "TSTAMP_SRC_SOE"},
    {0x00100000, "LAST"},
    {0x00800000, "REQUEST_FD"},
};

const FlagName kCtrlFlagNames[] = {
    {0x0001, "DISABLED"},
    {0x0002, "GRABBED"},
    {0x0004, "READ_ONLY"},
    {0x0008, "UPDATE"},
    {0x0010, "INACTIVE"},
    {0x0020, "SLIDER"},
    {0x0040, "WRITE_ONLY"},
    {0x0080, "VOLATILE"},
    {0x0100, "HAS_PAYLOAD"},
    {0x0200, "EXECUTE_ON_WRITE"},
    {0x0400, "MODIFY_LAYOUT"},
    {0x0800, "DYNAMIC_ARRAY"},
    {0x1000, "HAS_WHICH_MIN_MAX"},
};

template <size_t N>
QStringList flagNames(uint32_t value, const FlagName (&table)[N])
{
    QStringList out;
    uint32_t known = 0;
    for (const FlagName& f : table) {
        if (value & f.bit) {
            out << QString::fromLatin1(f.name);
        }
        known |= f.bit;
    }
    const uint32_t rest = value & ~known;
    if (rest != 0) {
        out << (QStringLiteral("0x") + QString::number(rest, 16));
    }
    return out;
}

template <size_t N>
QString flagText(uint32_t value, const FlagName (&table)[N])
{
    const QStringList names = flagNames(value, table);
    return names.isEmpty() ? QStringLiteral("-") : names.join(QLatin1Char('|'));
}

// ── 小工具 ──

int xioctl(int fd, unsigned long request, void* arg)
{
    int ret;
    do {
        ret = ::ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

QString msText(double ms)
{
    return QString::number(ms, 'f', 1) + QStringLiteral("ms");
}

const char* errnoName(int err)
{
    switch (err) {
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case EINTR: return "EINTR";
    case EIO: return "EIO";
    case ENXIO: return "ENXIO";
    case EBADF: return "EBADF";
    case EAGAIN: return "EAGAIN";
    case ENOMEM: return "ENOMEM";
    case EACCES: return "EACCES";
    case EFAULT: return "EFAULT";
    case EBUSY: return "EBUSY";
    case ENODEV: return "ENODEV";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case EINVAL: return "EINVAL";
    case ENOSPC: return "ENOSPC";
    case EPIPE: return "EPIPE";
    case ENOTTY: return "ENOTTY";
    case ENOSYS: return "ENOSYS";
    case ENODATA: return "ENODATA";
    case EOPNOTSUPP: return "EOPNOTSUPP";
    case ETIMEDOUT: return "ETIMEDOUT";
    default: return "E?";
    }
}

QString errText(int err)
{
    return QString::fromLatin1(errnoName(err)) + QLatin1Char('(') + QString::number(err) + QLatin1Char(')');
}

QJsonObject errJson(int err)
{
    QJsonObject o;
    o[QStringLiteral("errno")] = err;
    o[QStringLiteral("errnoName")] = QString::fromLatin1(errnoName(err));
    o[QStringLiteral("error")] = QString::fromLocal8Bit(std::strerror(err));
    return o;
}

QString hex32(uint32_t v)
{
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(8, QLatin1Char('0'));
}

// V4L2 struct 內的定長字元陣列（不一定以 NUL 結尾）
QString fixedStr(const void* p, size_t n)
{
    const char* s = static_cast<const char*>(p);
    return QString::fromUtf8(s, static_cast<qsizetype>(::strnlen(s, n))).trimmed();
}

QString fourccStr(uint32_t f)
{
    QString s;
    for (int i = 0; i < 4; ++i) {
        uint32_t c = (f >> (8 * i)) & 0xFF;
        if (i == 3) {
            c &= 0x7F;  // bit 31 是 big-endian 旗標（v4l2_fourcc_be）
        }
        s += (c >= 0x20 && c < 0x7F) ? QLatin1Char(static_cast<char>(c)) : QLatin1Char('.');
    }
    while (s.endsWith(QLatin1Char(' '))) {
        s.chop(1);
    }
    if (f & (1u << 31)) {
        s += QStringLiteral("-BE");
    }
    return s;
}

QString kernelVersionStr(uint32_t v)
{
    return QString::number((v >> 16) & 0xFF) + QLatin1Char('.') + QString::number((v >> 8) & 0xFF) +
           QLatin1Char('.') + QString::number(v & 0xFF);
}

QString groupName(gid_t gid)
{
    const struct group* g = ::getgrgid(gid);
    return (g != nullptr && g->gr_name != nullptr) ? QString::fromLocal8Bit(g->gr_name) : QString();
}

// 列出 dirPath 下「prefix + 純數字」的項目，依數字排序（video10 排在 video9 之後）。
// 只收純數字結尾，避免把 udev 建的別名連結重複算進來。
QStringList listNumberedEntries(const char* dirPath, const char* prefix)
{
    std::vector<std::pair<long, QString>> found;
    DIR* dir = ::opendir(dirPath);
    if (dir == nullptr) {
        return {};
    }
    const size_t prefixLen = std::strlen(prefix);
    while (const dirent* e = ::readdir(dir)) {
        const char* name = e->d_name;
        if (std::strncmp(name, prefix, prefixLen) != 0) {
            continue;
        }
        const char* digits = name + prefixLen;
        if (*digits == '\0' || std::strlen(digits) > 6) {
            continue;
        }
        bool allDigits = true;
        for (const char* c = digits; *c != '\0'; ++c) {
            if (*c < '0' || *c > '9') {
                allDigits = false;
                break;
            }
        }
        if (allDigits) {
            found.emplace_back(std::strtol(digits, nullptr, 10), QString::fromLocal8Bit(name));
        }
    }
    ::closedir(dir);
    std::sort(found.begin(), found.end(),
              [](const std::pair<long, QString>& a, const std::pair<long, QString>& b) { return a.first < b.first; });
    QStringList out;
    for (const auto& f : found) {
        out << f.second;
    }
    return out;
}

QString readSysfsName(const QString& node)
{
    QFile f(QStringLiteral("/sys/class/video4linux/") + node + QStringLiteral("/name"));
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.read(256)).trimmed();
}

// --device 可能給 by-path 之類的連結：先解析成實際節點再查 sysfs
QString sysfsNameForPath(const QString& path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    const QString node = QFileInfo(canonical.isEmpty() ? path : canonical).fileName();
    if (!node.startsWith(QLatin1String("video"))) {
        return QString();
    }
    return readSysfsName(node);
}

// 判斷 m2m：新式看 VIDEO_M2M／M2M_MPLANE；舊式 driver（例如較舊的 Qualcomm 下游 vidc）只宣告
// CAPTURE+OUTPUT(+STREAMING) 的組合，FFmpeg v4l2_m2m.c 也接受，而 v4l2m2m 才是實際的解碼
// 路徑，所以這裡照它判斷（legacyOut 標出來，判讀時分得清）。
void applyCaps(V4l2Caps::DeviceSummary& s, const v4l2_capability& cap, bool* legacyOut)
{
    s.driver = fixedStr(cap.driver, sizeof(cap.driver));
    s.card = fixedStr(cap.card, sizeof(cap.card));
    s.busInfo = fixedStr(cap.bus_info, sizeof(cap.bus_info));
    s.deviceCaps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

    const uint32_t c = s.deviceCaps;
    const bool streaming = (c & V4L2_CAP_STREAMING) != 0;
    const bool modernMplane = (c & V4L2_CAP_VIDEO_M2M_MPLANE) != 0;
    const bool modernSplane = (c & V4L2_CAP_VIDEO_M2M) != 0;
    const bool legacyMplane = streaming && (c & V4L2_CAP_VIDEO_CAPTURE_MPLANE) && (c & V4L2_CAP_VIDEO_OUTPUT_MPLANE);
    const bool legacySplane = streaming && (c & V4L2_CAP_VIDEO_CAPTURE) && (c & V4L2_CAP_VIDEO_OUTPUT);

    s.m2mMplane = modernMplane || legacyMplane;
    s.m2m = s.m2mMplane || modernSplane || legacySplane;
    if (legacyOut != nullptr) {
        *legacyOut = !modernMplane && !modernSplane && (legacyMplane || legacySplane);
    }
}

struct FdCloser {
    int fd;
    ~FdCloser()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

// ── 開檔失敗的診斷：權限還是沙箱 ──

QString modeString(mode_t m)
{
    QString s;
    s += S_ISCHR(m) ? QLatin1Char('c') : S_ISBLK(m) ? QLatin1Char('b') : S_ISDIR(m) ? QLatin1Char('d')
       : S_ISREG(m) ? QLatin1Char('-') : QLatin1Char('?');
    const char rwx[] = "rwxrwxrwx";
    for (int i = 0; i < 9; ++i) {
        s += (m & (0400 >> i)) ? QLatin1Char(rwx[i]) : QLatin1Char('-');
    }
    return s;
}

#ifdef VIPLE_HAVE_XATTR
// POSIX ACL 的 xattr 格式（linux/posix_acl_xattr.h，little-endian）：u32 版本 2，之後每筆
// {u16 tag, u16 perm, u32 id}。systemd-logind 的 uaccess 就是以 ACL_USER 項目把權限給目前 seat
// 的使用者；SSH 登入沒有 seat 就沒有這一項（scout E UNVERIFIED #10 要看的正是這個）。
QJsonArray aclEntriesJson(const char* path, QString* text)
{
    QJsonArray out;
    QStringList parts;
    unsigned char buf[1024];
    const ssize_t n = ::getxattr(path, "system.posix_acl_access", buf, sizeof(buf));
    if (n < 4) {
        return out;
    }
    auto le16 = [&buf](ssize_t off) {
        return static_cast<uint16_t>(buf[off] | (buf[off + 1] << 8));
    };
    auto le32 = [&buf](ssize_t off) {
        return static_cast<uint32_t>(buf[off]) | (static_cast<uint32_t>(buf[off + 1]) << 8) |
               (static_cast<uint32_t>(buf[off + 2]) << 16) | (static_cast<uint32_t>(buf[off + 3]) << 24);
    };
    if (le32(0) != 2) {
        return out;
    }
    for (ssize_t off = 4; off + 8 <= n; off += 8) {
        const uint16_t tag = le16(off);
        const uint16_t perm = le16(off + 2);
        const uint32_t id = le32(off + 4);
        const char* tagName = nullptr;
        switch (tag) {
        case 0x02: tagName = "user"; break;   // ACL_USER
        case 0x08: tagName = "group"; break;  // ACL_GROUP
        case 0x10: tagName = "mask"; break;   // ACL_MASK
        default: continue;                    // USER_OBJ／GROUP_OBJ／OTHER 與 mode 重複
        }
        QString permStr;
        permStr += (perm & 4) ? QLatin1Char('r') : QLatin1Char('-');
        permStr += (perm & 2) ? QLatin1Char('w') : QLatin1Char('-');
        permStr += (perm & 1) ? QLatin1Char('x') : QLatin1Char('-');
        QJsonObject e;
        e[QStringLiteral("tag")] = QString::fromLatin1(tagName);
        if (tag != 0x10) {
            e[QStringLiteral("id")] = static_cast<qint64>(id);
        }
        e[QStringLiteral("perm")] = permStr;
        out.append(e);
        parts << (tag == 0x10 ? QStringLiteral("mask:") + permStr
                              : QString::fromLatin1(tagName) + QLatin1Char(':') + QString::number(id) +
                                    QLatin1Char(':') + permStr);
    }
    *text = parts.join(QLatin1Char(','));
    return out;
}
#endif

QJsonObject nodeStatJson(const QString& path, QString* text)
{
    const QByteArray native = QFile::encodeName(path);
    struct stat st;
    if (::stat(native.constData(), &st) != 0) {
        const int err = errno;
        *text = QStringLiteral("stat=") + errText(err);
        return errJson(err);
    }
    QJsonObject o;
    const QString mode = modeString(st.st_mode);
    const QString grp = groupName(st.st_gid);
    o[QStringLiteral("mode")] = mode;
    o[QStringLiteral("uid")] = static_cast<qint64>(st.st_uid);
    o[QStringLiteral("gid")] = static_cast<qint64>(st.st_gid);
    if (!grp.isEmpty()) {
        o[QStringLiteral("group")] = grp;
    }
    if (S_ISCHR(st.st_mode)) {
        o[QStringLiteral("rdev")] = QString::number(major(st.st_rdev)) + QLatin1Char(':') + QString::number(minor(st.st_rdev));
    }
    const bool rw = ::access(native.constData(), R_OK | W_OK) == 0;
    o[QStringLiteral("accessRW")] = rw;

    *text = QStringLiteral("mode=") + mode + QStringLiteral(" owner=") + QString::number(st.st_uid) + QLatin1Char(':') +
            QString::number(st.st_gid) + (grp.isEmpty() ? QString() : QLatin1Char('(') + grp + QLatin1Char(')')) +
            QStringLiteral(" accessRW=") + (rw ? QStringLiteral("yes") : QStringLiteral("no"));
#ifdef VIPLE_HAVE_XATTR
    QString aclText;
    const QJsonArray acl = aclEntriesJson(native.constData(), &aclText);
    if (!acl.isEmpty()) {
        o[QStringLiteral("acl")] = acl;
        *text += QStringLiteral(" acl=[") + aclText + QLatin1Char(']');
    }
#endif
    return o;
}

QJsonObject processIdentityJson(QString* groupsText)
{
    QJsonObject o;
    o[QStringLiteral("euid")] = static_cast<qint64>(::geteuid());
    o[QStringLiteral("egid")] = static_cast<qint64>(::getegid());
    QJsonArray groups;
    QStringList text;
    const int n = ::getgroups(0, nullptr);
    if (n > 0) {
        std::vector<gid_t> gids(static_cast<size_t>(n));
        const int m = ::getgroups(n, gids.data());
        for (int i = 0; i < m; ++i) {
            const QString name = groupName(gids[static_cast<size_t>(i)]);
            QJsonObject g;
            g[QStringLiteral("gid")] = static_cast<qint64>(gids[static_cast<size_t>(i)]);
            if (!name.isEmpty()) {
                g[QStringLiteral("name")] = name;
            }
            groups.append(g);
            text << (name.isEmpty() ? QString::number(gids[static_cast<size_t>(i)])
                                    : QString::number(gids[static_cast<size_t>(i)]) + QLatin1Char('(') + name + QLatin1Char(')'));
        }
    }
    o[QStringLiteral("groups")] = groups;
    *groupsText = text.join(QLatin1Char(','));
    return o;
}

QString openHint(int err)
{
    switch (err) {
    case ENOENT:
        return ProbeUtil::isInFlatpak() ? QStringLiteral("node not visible inside the sandbox (Flatpak needs --device=all)")
                                        : QStringLiteral("node does not exist");
    case EACCES:
    case EPERM:
        return QStringLiteral("permission denied: needs the node's group (video/render) or a seat ACL "
                              "(uaccess; SSH logins usually have no seat)");
    case EBUSY:
        return QStringLiteral("device busy");
    case ENODEV:
    case ENXIO:
        return QStringLiteral("node exists but the device is not available");
    default:
        return QString();
    }
}

// ── 格式列舉 ──

struct FmtDesc {
    uint32_t fourcc = 0;
    uint32_t flags = 0;
    QString description;
};

std::vector<FmtDesc> enumFormats(int fd, uint32_t type, int* firstErr)
{
    std::vector<FmtDesc> out;
    *firstErr = 0;
    for (int i = 0; i < kMaxEnum; ++i) {
        v4l2_fmtdesc d{};
        d.index = static_cast<uint32_t>(i);
        d.type = type;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &d) != 0) {
            if (i == 0) {
                *firstErr = errno;  // EINVAL = 沒有任何格式
            }
            break;
        }
        FmtDesc f;
        f.fourcc = d.pixelformat;
        f.flags = d.flags;
        f.description = fixedStr(d.description, sizeof(d.description));
        out.push_back(f);
    }
    return out;
}

QJsonObject fmtJson(const FmtDesc& f)
{
    QJsonObject o;
    o[QStringLiteral("fourcc")] = fourccStr(f.fourcc);
    o[QStringLiteral("fourccHex")] = hex32(f.fourcc);
    o[QStringLiteral("description")] = f.description;
    o[QStringLiteral("flags")] = QJsonArray::fromStringList(flagNames(f.flags, kFmtFlagNames));
    const bool compressed = (f.flags & V4L2_FMT_FLAG_COMPRESSED) != 0;
    o[QStringLiteral("compressed")] = compressed;
    if (compressed) {
        if (const CodecInfo* c = findCodec(f.fourcc)) {
            o[QStringLiteral("codec")] = QString::fromLatin1(c->name);
            o[QStringLiteral("stateless")] = c->stateless;
        }
        else if (isNonVideoCoded(f.fourcc)) {
            o[QStringLiteral("nonVideoCoded")] = true;
        }
    }
    return o;
}

QJsonObject frameSizesJson(int fd, uint32_t fourcc, QString* text)
{
    v4l2_frmsizeenum fs{};
    fs.index = 0;
    fs.pixel_format = fourcc;
    if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) {
        const int err = errno;
        *text = QStringLiteral("?(") + errText(err) + QLatin1Char(')');
        return errJson(err);
    }
    QJsonObject o;
    if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        QJsonArray sizes;
        QStringList t;
        for (int i = 0; i < kMaxEnum; ++i) {
            if (i > 0) {
                fs = v4l2_frmsizeenum{};
                fs.index = static_cast<uint32_t>(i);
                fs.pixel_format = fourcc;
                if (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0 || fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) {
                    break;
                }
            }
            const QString s = QString::number(fs.discrete.width) + QLatin1Char('x') + QString::number(fs.discrete.height);
            sizes.append(s);
            t << s;
        }
        o[QStringLiteral("type")] = QStringLiteral("discrete");
        o[QStringLiteral("sizes")] = sizes;
        *text = t.join(QLatin1Char(','));
    }
    else {
        const v4l2_frmsize_stepwise& sw = fs.stepwise;
        o[QStringLiteral("type")] = fs.type == V4L2_FRMSIZE_TYPE_CONTINUOUS ? QStringLiteral("continuous")
                                                                           : QStringLiteral("stepwise");
        o[QStringLiteral("minWidth")] = static_cast<qint64>(sw.min_width);
        o[QStringLiteral("maxWidth")] = static_cast<qint64>(sw.max_width);
        o[QStringLiteral("stepWidth")] = static_cast<qint64>(sw.step_width);
        o[QStringLiteral("minHeight")] = static_cast<qint64>(sw.min_height);
        o[QStringLiteral("maxHeight")] = static_cast<qint64>(sw.max_height);
        o[QStringLiteral("stepHeight")] = static_cast<qint64>(sw.step_height);
        *text = QString::number(sw.min_width) + QLatin1Char('x') + QString::number(sw.min_height) + QStringLiteral("..") +
                QString::number(sw.max_width) + QLatin1Char('x') + QString::number(sw.max_height);
    }
    return o;
}

// REQBUFS(count=0) 不配置記憶體，只回報這個 queue 支援的記憶體型別等能力（kernel 4.20 起）。
// 但它仍會動到 queue 狀態：非 vb2／非 m2m 的驅動（例如 exclusive_caps=0 的 v4l2loopback ≤0.13）
// 會重設所有 opener 共用的 buffer 旗標，正在讀它的程式之後每次 DQBUF 都失敗。所以呼叫端只對
// decoder、而且不是舊式旗標判定出來的節點做。
QJsonObject queueCapsJson(int fd, uint32_t type, QString* text)
{
    v4l2_requestbuffers rb{};
    rb.count = 0;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &rb) != 0) {
        const int err = errno;
        *text = errText(err);
        return errJson(err);
    }
    QJsonObject o;
#ifdef V4L2_BUF_CAP_SUPPORTS_MMAP
    o[QStringLiteral("raw")] = hex32(rb.capabilities);
    o[QStringLiteral("names")] = QJsonArray::fromStringList(flagNames(rb.capabilities, kBufCapNames));
    *text = rb.capabilities != 0 ? QLatin1Char('[') + flagNames(rb.capabilities, kBufCapNames).join(QLatin1Char(',')) + QLatin1Char(']')
                                 : QStringLiteral("[?]");
#else
    o[QStringLiteral("raw")] = QStringLiteral("unavailable (old kernel headers)");
    *text = QStringLiteral("[?]");
#endif
    return o;
}

void releaseBuffers(int fd, uint32_t type)
{
    v4l2_requestbuffers rb{};
    rb.count = 0;
    rb.type = type;
    rb.memory = V4L2_MEMORY_MMAP;
    xioctl(fd, VIDIOC_REQBUFS, &rb);
}

// ── S_FMT OUTPUT／G_FMT CAPTURE ──

struct OutFmtResult {
    bool ok = false;
    int err = 0;
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sizeimage = 0;
    uint32_t numPlanes = 1;
};

OutFmtResult setOutputFormat(int fd, bool mplane, uint32_t fourcc, uint32_t width, uint32_t height)
{
    v4l2_format f{};
    if (mplane) {
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        f.fmt.pix_mp.width = width;
        f.fmt.pix_mp.height = height;
        f.fmt.pix_mp.pixelformat = fourcc;
        f.fmt.pix_mp.field = V4L2_FIELD_NONE;
        f.fmt.pix_mp.num_planes = 1;
        f.fmt.pix_mp.plane_fmt[0].sizeimage = kCodedSizeImage;
    }
    else {
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        f.fmt.pix.width = width;
        f.fmt.pix.height = height;
        f.fmt.pix.pixelformat = fourcc;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        f.fmt.pix.sizeimage = kCodedSizeImage;
    }
    OutFmtResult r;
    if (xioctl(fd, VIDIOC_S_FMT, &f) != 0) {
        r.err = errno;
        return r;
    }
    r.ok = true;
    if (mplane) {
        r.fourcc = f.fmt.pix_mp.pixelformat;
        r.width = f.fmt.pix_mp.width;
        r.height = f.fmt.pix_mp.height;
        r.sizeimage = f.fmt.pix_mp.plane_fmt[0].sizeimage;
        r.numPlanes = std::clamp<uint32_t>(f.fmt.pix_mp.num_planes, 1, VIDEO_MAX_PLANES);
    }
    else {
        r.fourcc = f.fmt.pix.pixelformat;
        r.width = f.fmt.pix.width;
        r.height = f.fmt.pix.height;
        r.sizeimage = f.fmt.pix.sizeimage;
        r.numPlanes = 1;
    }
    return r;
}

QJsonObject outFmtJson(const OutFmtResult& r)
{
    if (!r.ok) {
        return errJson(r.err);
    }
    QJsonObject o;
    o[QStringLiteral("fourcc")] = fourccStr(r.fourcc);
    o[QStringLiteral("width")] = static_cast<qint64>(r.width);
    o[QStringLiteral("height")] = static_cast<qint64>(r.height);
    o[QStringLiteral("sizeimage")] = static_cast<qint64>(r.sizeimage);
    o[QStringLiteral("numPlanes")] = static_cast<qint64>(r.numPlanes);
    return o;
}

struct CapFmt {
    bool ok = false;
    int err = 0;
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t field = 0;
    uint32_t colorspace = 0;
    uint32_t numPlanes = 1;
    uint32_t bytesperline[VIDEO_MAX_PLANES] = {};
    uint32_t sizeimage[VIDEO_MAX_PLANES] = {};
};

CapFmt getCaptureFormat(int fd, bool mplane)
{
    v4l2_format f{};
    f.type = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
    CapFmt r;
    if (xioctl(fd, VIDIOC_G_FMT, &f) != 0) {
        r.err = errno;
        return r;
    }
    r.ok = true;
    if (mplane) {
        r.fourcc = f.fmt.pix_mp.pixelformat;
        r.width = f.fmt.pix_mp.width;
        r.height = f.fmt.pix_mp.height;
        r.field = f.fmt.pix_mp.field;
        r.colorspace = f.fmt.pix_mp.colorspace;
        r.numPlanes = std::clamp<uint32_t>(f.fmt.pix_mp.num_planes, 1, VIDEO_MAX_PLANES);
        for (uint32_t p = 0; p < r.numPlanes; ++p) {
            r.bytesperline[p] = f.fmt.pix_mp.plane_fmt[p].bytesperline;
            r.sizeimage[p] = f.fmt.pix_mp.plane_fmt[p].sizeimage;
        }
    }
    else {
        r.fourcc = f.fmt.pix.pixelformat;
        r.width = f.fmt.pix.width;
        r.height = f.fmt.pix.height;
        r.field = f.fmt.pix.field;
        r.colorspace = f.fmt.pix.colorspace;
        r.numPlanes = 1;
        r.bytesperline[0] = f.fmt.pix.bytesperline;
        r.sizeimage[0] = f.fmt.pix.sizeimage;
    }
    return r;
}

QJsonObject capFmtJson(const CapFmt& c)
{
    QJsonObject o;
    o[QStringLiteral("fourcc")] = fourccStr(c.fourcc);
    o[QStringLiteral("fourccHex")] = hex32(c.fourcc);
    o[QStringLiteral("width")] = static_cast<qint64>(c.width);
    o[QStringLiteral("height")] = static_cast<qint64>(c.height);
    o[QStringLiteral("field")] = static_cast<qint64>(c.field);
    o[QStringLiteral("colorspace")] = static_cast<qint64>(c.colorspace);
    o[QStringLiteral("numPlanes")] = static_cast<qint64>(c.numPlanes);
    QJsonArray planes;
    for (uint32_t p = 0; p < c.numPlanes; ++p) {
        QJsonObject po;
        po[QStringLiteral("bytesperline")] = static_cast<qint64>(c.bytesperline[p]);
        po[QStringLiteral("sizeimage")] = static_cast<qint64>(c.sizeimage[p]);
        planes.append(po);
    }
    o[QStringLiteral("planes")] = planes;
    return o;
}

// 可見區域。選取 API 的規格要求用非 _MPLANE 的 type；kernel 4.13 起也接受 _MPLANE，兩個都試。
QJsonObject composeJson(int fd, bool mplane, QString* text)
{
    const uint32_t types[2] = {V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE};
    int lastErr = 0;
    for (int i = 0; i < (mplane ? 2 : 1); ++i) {
        v4l2_selection sel{};
        sel.type = types[i];
        sel.target = V4L2_SEL_TGT_COMPOSE;
        if (xioctl(fd, VIDIOC_G_SELECTION, &sel) == 0) {
            QJsonObject o;
            o[QStringLiteral("left")] = sel.r.left;
            o[QStringLiteral("top")] = sel.r.top;
            o[QStringLiteral("width")] = static_cast<qint64>(sel.r.width);
            o[QStringLiteral("height")] = static_cast<qint64>(sel.r.height);
            o[QStringLiteral("typeUsed")] = i == 0 ? QStringLiteral("VIDEO_CAPTURE") : QStringLiteral("VIDEO_CAPTURE_MPLANE");
            *text = QString::number(sel.r.width) + QLatin1Char('x') + QString::number(sel.r.height);
            return o;
        }
        lastErr = errno;
    }
    *text = QStringLiteral("?(") + errText(lastErr) + QLatin1Char(')');
    return errJson(lastErr);
}

// ── 控制項 ──

struct MenuItem {
    int index = 0;
    QString name;
};

struct CtrlInfo {
    uint32_t id = 0;
    uint32_t type = 0;
    QString name;
    int32_t minimum = 0;
    int32_t maximum = 0;
    int32_t step = 0;
    int32_t defaultValue = 0;
    uint32_t flags = 0;
    bool hasValue = false;
    int32_t value = 0;
    std::vector<MenuItem> menu;
};

QString ctrlTypeName(uint32_t type)
{
    switch (type) {
    case kCtrlInteger: return QStringLiteral("integer");
    case kCtrlBoolean: return QStringLiteral("boolean");
    case kCtrlMenu: return QStringLiteral("menu");
    case kCtrlButton: return QStringLiteral("button");
    case kCtrlInteger64: return QStringLiteral("integer64");
    case kCtrlClass: return QStringLiteral("class");
    case kCtrlString: return QStringLiteral("string");
    case kCtrlBitmask: return QStringLiteral("bitmask");
    case kCtrlIntegerMenu: return QStringLiteral("integer_menu");
    default: return QStringLiteral("compound(") + hex32(type) + QLatin1Char(')');
    }
}

// 用 NEXT_CTRL|NEXT_COMPOUND 走完全部控制項；menu 型別逐項 QUERYMENU（menu_skip_mask 排除的
// 項目回 EINVAL，略過即可）；可讀的純量控制項順便 G_CTRL 取目前值。
std::vector<CtrlInfo> walkControls(int fd, int* firstErr)
{
    std::vector<CtrlInfo> out;
    *firstErr = 0;
    uint32_t nextFlags = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    uint32_t lastId = 0;
    bool retried = false;
    while (static_cast<int>(out.size()) < kMaxControls) {
        v4l2_queryctrl qc{};
        qc.id = lastId | nextFlags;
        if (xioctl(fd, VIDIOC_QUERYCTRL, &qc) != 0) {
            const int err = errno;
            if (out.empty() && !retried && (nextFlags & V4L2_CTRL_FLAG_NEXT_COMPOUND)) {
                // 3.17 以前的 kernel 不認得 NEXT_COMPOUND：退回只走一般控制項
                nextFlags = V4L2_CTRL_FLAG_NEXT_CTRL;
                retried = true;
                continue;
            }
            if (out.empty()) {
                *firstErr = err;  // EINVAL = 沒有控制項；ENOTTY = 不支援
            }
            break;
        }
        if (qc.id <= lastId) {
            break;  // driver 沒有往前走：停下，避免無窮迴圈
        }
        lastId = qc.id;

        CtrlInfo c;
        c.id = qc.id;
        c.type = qc.type;
        c.name = fixedStr(qc.name, sizeof(qc.name));
        c.minimum = qc.minimum;
        c.maximum = qc.maximum;
        c.step = qc.step;
        c.defaultValue = qc.default_value;
        c.flags = qc.flags;

        const bool disabled = (qc.flags & V4L2_CTRL_FLAG_DISABLED) != 0;
        if (!disabled && (qc.type == kCtrlMenu || qc.type == kCtrlIntegerMenu)) {
            int tries = 0;
            for (int64_t idx = qc.minimum; idx <= qc.maximum && tries < kMaxMenuItems; ++idx, ++tries) {
                if (idx < 0) {
                    continue;
                }
                v4l2_querymenu qm{};
                qm.id = qc.id;
                qm.index = static_cast<uint32_t>(idx);
                if (xioctl(fd, VIDIOC_QUERYMENU, &qm) != 0) {
                    continue;
                }
                MenuItem mi;
                mi.index = static_cast<int>(idx);
                mi.name = qc.type == kCtrlMenu ? fixedStr(qm.name, sizeof(qm.name))
                                               : QString::number(static_cast<qlonglong>(qm.value));
                c.menu.push_back(mi);
            }
        }

        const bool scalar = qc.type == kCtrlInteger || qc.type == kCtrlBoolean || qc.type == kCtrlMenu ||
                            qc.type == kCtrlIntegerMenu || qc.type == kCtrlBitmask;
        if (!disabled && scalar && !(qc.flags & V4L2_CTRL_FLAG_WRITE_ONLY)) {
            v4l2_control v{};
            v.id = qc.id;
            if (xioctl(fd, VIDIOC_G_CTRL, &v) == 0) {
                c.hasValue = true;
                c.value = v.value;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

const CtrlInfo* findCtrl(const std::vector<CtrlInfo>& ctrls, uint32_t id)
{
    if (id == 0) {
        return nullptr;
    }
    for (const CtrlInfo& c : ctrls) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

QJsonObject ctrlJson(const CtrlInfo& c)
{
    QJsonObject o;
    o[QStringLiteral("id")] = hex32(c.id);
    o[QStringLiteral("name")] = c.name;
    o[QStringLiteral("type")] = ctrlTypeName(c.type);
    if (c.type != kCtrlClass) {
        o[QStringLiteral("min")] = c.minimum;
        o[QStringLiteral("max")] = c.maximum;
        o[QStringLiteral("step")] = c.step;
        o[QStringLiteral("default")] = c.defaultValue;
    }
    if (c.flags != 0) {
        o[QStringLiteral("flags")] = QJsonArray::fromStringList(flagNames(c.flags, kCtrlFlagNames));
    }
    if (c.hasValue) {
        o[QStringLiteral("value")] = c.value;
    }
    if (!c.menu.empty()) {
        QJsonArray menu;
        for (const MenuItem& mi : c.menu) {
            QJsonObject m;
            m[QStringLiteral("index")] = mi.index;
            m[QStringLiteral("name")] = mi.name;
            menu.append(m);
        }
        o[QStringLiteral("menu")] = menu;
    }
    return o;
}

QStringList menuNames(const CtrlInfo* c)
{
    QStringList out;
    if (c != nullptr) {
        for (const MenuItem& mi : c->menu) {
            out << mi.name;
        }
    }
    return out;
}

// codec 的 profile／level／tier 摘要，外加 VR 串流解析度需要的 level 判斷。
// 3456x1728 是 vr_architecture.md 的 PCVR 串流解析度；推算見 scout E §3.1：
//   HEVC：90 Hz 的 luma 取樣率 537,477,120 > L5.1 上限 534,773,760 → 要 5.2；72 Hz 用 5.1 即可。
//   H.264：90 Hz 為 2,099,520 MB/s > L5.2 上限 2,073,600 → 要 6.0；72 Hz 用 5.2 即可。
QJsonObject codecSummaryJson(const CodecInfo& codec, const std::vector<CtrlInfo>& ctrls, QString* fragment)
{
    QJsonObject o;
    if (codec.profileCid != 0) {
        const CtrlInfo* c = findCtrl(ctrls, codec.profileCid);
        const QStringList names = menuNames(c);
        o[QStringLiteral("profiles")] = c != nullptr ? QJsonValue(QJsonArray::fromStringList(names)) : QJsonValue(QStringLiteral("n/a"));
        *fragment += QStringLiteral(" profiles=") + (c != nullptr ? QLatin1Char('[') + names.join(QLatin1Char(',')) + QLatin1Char(']')
                                                                  : QStringLiteral("n/a"));
    }
    if (codec.levelCid != 0) {
        const CtrlInfo* c = findCtrl(ctrls, codec.levelCid);
        if (c != nullptr && !c->menu.empty()) {
            const MenuItem top = *std::max_element(c->menu.begin(), c->menu.end(),
                                                   [](const MenuItem& a, const MenuItem& b) { return a.index < b.index; });
            o[QStringLiteral("levels")] = QJsonArray::fromStringList(menuNames(c));
            o[QStringLiteral("levelMax")] = top.name;
            o[QStringLiteral("levelMaxIndex")] = top.index;
            *fragment += QStringLiteral(" levelMax=") + top.name;

            int need90 = -1;
            int need72 = -1;
            QString req90;
            QString req72;
            if (codec.levelCid == kCidHevcLevel) {
                need90 = kHevcLevel52;
                need72 = kHevcLevel51;
                req90 = QStringLiteral("5.2");
                req72 = QStringLiteral("5.1");
            }
            else if (codec.levelCid == kCidH264Level) {
                need90 = kH264Level60;
                need72 = kH264Level52;
                req90 = QStringLiteral("6.0");
                req72 = QStringLiteral("5.2");
            }
            if (need90 >= 0) {
                QJsonObject vr;
                vr[QStringLiteral("target")] = QStringLiteral("3456x1728");
                vr[QStringLiteral("required90Hz")] = req90;
                vr[QStringLiteral("required72Hz")] = req72;
                vr[QStringLiteral("ok90Hz")] = top.index >= need90;
                vr[QStringLiteral("ok72Hz")] = top.index >= need72;
                o[QStringLiteral("vrLevelCheck")] = vr;
                *fragment += QStringLiteral(" vr90=") + (top.index >= need90 ? QStringLiteral("ok") : QStringLiteral("no(need ") + req90 + QLatin1Char(')')) +
                             QStringLiteral(" vr72=") + (top.index >= need72 ? QStringLiteral("ok") : QStringLiteral("no(need ") + req72 + QLatin1Char(')'));
            }
        }
        else {
            o[QStringLiteral("levelMax")] = QStringLiteral("n/a");
            *fragment += QStringLiteral(" levelMax=n/a");
        }
    }
    if (codec.tierCid != 0) {
        const CtrlInfo* c = findCtrl(ctrls, codec.tierCid);
        const QStringList names = menuNames(c);
        o[QStringLiteral("tiers")] = c != nullptr ? QJsonValue(QJsonArray::fromStringList(names)) : QJsonValue(QStringLiteral("n/a"));
        *fragment += QStringLiteral(" tiers=") + (c != nullptr ? QLatin1Char('[') + names.join(QLatin1Char(',')) + QLatin1Char(']')
                                                               : QStringLiteral("n/a"));
    }
    return o;
}

// ── decoder 指令與串流參數 ──

QString tryDecoderCmd(int fd, uint32_t cmd)
{
    v4l2_decoder_cmd dc{};
    dc.cmd = cmd;
    return xioctl(fd, VIDIOC_TRY_DECODER_CMD, &dc) == 0 ? QStringLiteral("ok") : errText(errno);
}

// G_PARM：L1 打算用 S_PARM 的 timeperframe 拉高 VPU 時脈（iris 以 OUTPUT 的幀率算 load），
// 先看 driver 有沒有 V4L2_CAP_TIMEPERFRAME
QJsonObject parmJson(int fd, uint32_t type, bool isOutput, QString* text)
{
    v4l2_streamparm sp{};
    sp.type = type;
    if (xioctl(fd, VIDIOC_G_PARM, &sp) != 0) {
        const int err = errno;
        *text = errText(err);
        return errJson(err);
    }
    const uint32_t capability = isOutput ? sp.parm.output.capability : sp.parm.capture.capability;
    const v4l2_fract tpf = isOutput ? sp.parm.output.timeperframe : sp.parm.capture.timeperframe;
    const bool supported = (capability & V4L2_CAP_TIMEPERFRAME) != 0;
    const QString tpfText = QString::number(tpf.numerator) + QLatin1Char('/') + QString::number(tpf.denominator);
    QJsonObject o;
    o[QStringLiteral("capability")] = hex32(capability);
    o[QStringLiteral("timeperframeSupported")] = supported;
    o[QStringLiteral("timeperframe")] = tpfText;
    *text = supported ? QStringLiteral("ok(timeperframe=") + tpfText + QLatin1Char(')') : QStringLiteral("ok(no-timeperframe)");
    return o;
}

// ── header-test ──

// 等 fd 出現 events 之一或到 deadline。回傳 revents（>0）、0（逾時）、-1（poll 失敗，errno 保留）。
// 只有 POLLERR/POLLHUP、沒有想要的事件時（部分 driver 的 poll 實作會這樣）睡 10 ms 再等，
// 不做忙迴圈；總時間仍受 deadline 限制。
int waitFd(int fd, short events, Clock::time_point deadline, int* errorWakeups)
{
    for (;;) {
        const Clock::time_point now = Clock::now();
        if (now >= deadline) {
            return 0;
        }
        const long long leftMs = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count() + 1;
        pollfd p{};
        p.fd = fd;
        p.events = events;
        const int n = ::poll(&p, 1, static_cast<int>(std::min<long long>(leftMs, kWaitMs + 1)));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            continue;  // 回到上面檢查 deadline
        }
        if (p.revents & events) {
            return p.revents;
        }
        if (p.revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        ++*errorWakeups;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 取出目前排隊的事件（DQEVENT 直到沒有，最多 kMaxEvents 個）。回傳取出的個數；
// POLLPRI 亮了卻取不到事件時（driver 自己的 poll 實作），呼叫端要稍睡再等，避免空轉。
int dequeueEvents(int fd, QJsonArray& log, bool* sourceChange, uint32_t* changes, bool* eos)
{
    int count = 0;
    for (int i = 0; i < kMaxEvents; ++i) {
        v4l2_event ev{};
        if (xioctl(fd, VIDIOC_DQEVENT, &ev) != 0) {
            break;
        }
        ++count;
        QJsonObject e;
        e[QStringLiteral("sequence")] = static_cast<qint64>(ev.sequence);
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
            e[QStringLiteral("type")] = QStringLiteral("SOURCE_CHANGE");
            e[QStringLiteral("changes")] = hex32(ev.u.src_change.changes);
            if (sourceChange != nullptr) {
                *sourceChange = true;
            }
            if (changes != nullptr) {
                *changes |= ev.u.src_change.changes;
            }
        }
        else if (ev.type == V4L2_EVENT_EOS) {
            e[QStringLiteral("type")] = QStringLiteral("EOS");
            if (eos != nullptr) {
                *eos = true;
            }
        }
        else {
            e[QStringLiteral("type")] = hex32(ev.type);
        }
        log.append(e);
        if (ev.pending == 0) {
            break;
        }
    }
    return count;
}

void shortBackoff()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

QJsonObject expbufJson(int fd, uint32_t capType, uint32_t numPlanes, QString* text)
{
    QJsonObject o;
    QJsonArray planes;
    bool allOk = true;
    int firstErr = 0;
    for (uint32_t p = 0; p < numPlanes; ++p) {
        v4l2_exportbuffer eb{};
        eb.type = capType;
        eb.index = 0;
        eb.plane = p;
        eb.flags = O_CLOEXEC | O_RDONLY;
        QJsonObject po;
        po[QStringLiteral("plane")] = static_cast<qint64>(p);
        if (xioctl(fd, VIDIOC_EXPBUF, &eb) != 0) {
            const int err = errno;
            const QJsonObject e = errJson(err);
            for (auto it = e.begin(); it != e.end(); ++it) {
                po[it.key()] = it.value();
            }
            po[QStringLiteral("ok")] = false;
            if (allOk) {
                firstErr = err;
            }
            allOk = false;
        }
        else {
            po[QStringLiteral("ok")] = true;
            const off_t size = ::lseek(eb.fd, 0, SEEK_END);
            if (size >= 0) {
                po[QStringLiteral("dmabufBytes")] = static_cast<qint64>(size);
            }
            ::close(eb.fd);
        }
        planes.append(po);
    }
    o[QStringLiteral("ok")] = allOk;
    o[QStringLiteral("planes")] = planes;
    o[QStringLiteral("flags")] = QStringLiteral("O_CLOEXEC|O_RDONLY");
    *text = allOk ? QStringLiteral("ok") : errText(firstErr);
    return o;
}

// header-test 的資源。解構順序：先停兩個 queue 的串流，再解除 OUTPUT 的 mmap（buffer 仍被映射時
// vb2 的 REQBUFS(0) 會回 EBUSY），再釋放兩邊的 buffer，最後關檔。所有離開路徑都走這裡。
struct HeaderTestResources {
    int fd = -1;
    uint32_t outType = 0;
    uint32_t capType = 0;
    void* maps[kOutputBuffers] = {MAP_FAILED, MAP_FAILED};
    size_t mapLengths[kOutputBuffers] = {0, 0};

    HeaderTestResources() = default;
    HeaderTestResources(const HeaderTestResources&) = delete;
    HeaderTestResources& operator=(const HeaderTestResources&) = delete;

    ~HeaderTestResources()
    {
        if (fd < 0) {
            return;
        }
        int type = static_cast<int>(capType);
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        type = static_cast<int>(outType);
        xioctl(fd, VIDIOC_STREAMOFF, &type);
        for (int i = 0; i < kOutputBuffers; ++i) {
            if (maps[i] != MAP_FAILED) {
                ::munmap(maps[i], mapLengths[i]);
                maps[i] = MAP_FAILED;
            }
        }
        releaseBuffers(fd, capType);
        releaseBuffers(fd, outType);
        ::close(fd);
        fd = -1;
    }
};

// 依 stateful decoder 規格的初始化流程跑一次：S_FMT OUTPUT → SUBSCRIBE SOURCE_CHANGE →
// REQBUFS OUTPUT（兩個 buffer 都先寫好同一個測試 AU，但 STREAMON 前只把第一份入列）→ STREAMON
// OUTPUT → 等 SOURCE_CHANGE（1 s 等不到才補送第二份再等 1 s）→ G_FMT/G_SELECTION CAPTURE、串流後的
// MIN_BUFFERS_FOR_CAPTURE → REQBUFS/(EXPBUF)/QBUF CAPTURE → STREAMON CAPTURE → 分三段等第一幀，
// 結果記在 firstFrame.outputAfter（見第 9 步）。最壞約 4 s。
QJsonObject runHeaderTest(const QString& path, bool mplane, const CodecInfo& codec,
                          const uint8_t* frame, size_t frameLen, bool expbuf, QString* lineOut)
{
    QJsonObject r;
    r[QStringLiteral("codec")] = QString::fromLatin1(codec.headerTestKey);
    r[QStringLiteral("fourcc")] = fourccStr(codec.fourcc);
    r[QStringLiteral("testFrameBytes")] = static_cast<qint64>(frameLen);
    r[QStringLiteral("testFrameResolution")] = QString::number(kTestFrameWidth) + QLatin1Char('x') + QString::number(kTestFrameHeight);

    QStringList parts;  // stdout／log 那一行的片段
    QString failure;
    QJsonArray events;
    int errorWakeups = 0;
    bool gotFrame = false;
    bool frameErrorFlag = false;
    bool lateSourceChange = false;  // CAPTURE 串流之後才到的 SOURCE_CHANGE（和第 4 步的分開記）

    HeaderTestResources res;
    res.outType = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
    res.capType = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    auto fail = [&](const char* step, int err) {
        QJsonObject f = errJson(err);
        f[QStringLiteral("step")] = QString::fromLatin1(step);
        r[QStringLiteral("failure")] = f;
        failure = QStringLiteral("failed at ") + QString::fromLatin1(step) + QStringLiteral(": ") + errText(err);
    };

    // 任何一步失敗就 return；資源一律由 res 的解構子收尾
    [&]() {
        res.fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (res.fd < 0) {
            fail("open", errno);
            return;
        }

        // 1. OUTPUT 格式：coded 格式＋測試幀解析度（placeholder，driver 解析 SPS 後會更新）
        const OutFmtResult of = setOutputFormat(res.fd, mplane, codec.fourcc, kTestFrameWidth, kTestFrameHeight);
        r[QStringLiteral("outputFormat")] = outFmtJson(of);
        if (!of.ok) {
            fail("s_fmt(output)", of.err);
            return;
        }
        if (of.fourcc != codec.fourcc) {
            fail("s_fmt(output) substituted the coded format", EINVAL);
            return;
        }

        // 2. 事件：SOURCE_CHANGE 是必要的通知；EOS 只是記錄
        {
            v4l2_event_subscription sub{};
            sub.type = V4L2_EVENT_SOURCE_CHANGE;
            r[QStringLiteral("subscribeSourceChange")] =
                xioctl(res.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0 ? QStringLiteral("ok") : errText(errno);
            sub = v4l2_event_subscription{};
            sub.type = V4L2_EVENT_EOS;
            r[QStringLiteral("subscribeEos")] =
                xioctl(res.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0 ? QStringLiteral("ok") : errText(errno);
        }

        // 3. OUTPUT buffer：MMAP，每個都先寫好同一個測試 AU；STREAMON 前只把第一份入列。
        //    兩份一起入列的話，「要看到下一個 AU 才吐幀」的 decoder 在第一段就會出幀，和立即
        //    出幀分不出來（串流時一個 buffer 只放一個 AU，這類 decoder 每幀都會多等一個幀間隔）。
        v4l2_requestbuffers rb{};
        rb.count = kOutputBuffers;
        rb.type = res.outType;
        rb.memory = V4L2_MEMORY_MMAP;
        if (xioctl(res.fd, VIDIOC_REQBUFS, &rb) != 0) {
            fail("reqbufs(output)", errno);
            return;
        }
        if (rb.count == 0) {
            fail("reqbufs(output) allocated no buffers", ENOMEM);
            return;
        }
        const int outCount = std::min(static_cast<int>(rb.count), kOutputBuffers);
        QJsonObject ob;
        ob[QStringLiteral("requested")] = kOutputBuffers;
        ob[QStringLiteral("allocated")] = static_cast<qint64>(rb.count);
        for (int i = 0; i < outCount; ++i) {
            v4l2_buffer b{};
            v4l2_plane planes[VIDEO_MAX_PLANES] = {};
            b.index = static_cast<uint32_t>(i);
            b.type = res.outType;
            b.memory = V4L2_MEMORY_MMAP;
            if (mplane) {
                b.m.planes = planes;
                b.length = of.numPlanes;
            }
            if (xioctl(res.fd, VIDIOC_QUERYBUF, &b) != 0) {
                fail("querybuf(output)", errno);
                return;
            }
            const uint32_t length = mplane ? planes[0].length : b.length;
            const uint32_t offset = mplane ? planes[0].m.mem_offset : b.m.offset;
            ob[QStringLiteral("planeLength")] = static_cast<qint64>(length);
            if (length < frameLen) {
                r[QStringLiteral("outputBuffers")] = ob;
                fail("output buffer smaller than the test frame", ENOSPC);
                return;
            }
            void* addr = ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, res.fd, static_cast<off_t>(offset));
            if (addr == MAP_FAILED) {
                fail("mmap(output)", errno);
                return;
            }
            res.maps[i] = addr;
            res.mapLengths[i] = length;
            std::memcpy(addr, frame, frameLen);
        }
        // 第 auNo 份 AU（1 起算）放進 OUTPUT buffer index 入列。時間戳 auNo×1000 µs：m2m decoder
        // 會把時間戳原樣帶到對應的 CAPTURE buffer（TIMESTAMP_COPY）。回傳 0 或 QBUF 的 errno。
        auto queueOutput = [&](int index, int auNo) -> int {
            v4l2_buffer b{};
            v4l2_plane planes[VIDEO_MAX_PLANES] = {};
            b.index = static_cast<uint32_t>(index);
            b.type = res.outType;
            b.memory = V4L2_MEMORY_MMAP;
            b.field = V4L2_FIELD_NONE;
            b.timestamp.tv_sec = 0;
            b.timestamp.tv_usec = auNo * 1000;
            if (mplane) {
                planes[0].bytesused = static_cast<uint32_t>(frameLen);
                b.m.planes = planes;
                b.length = of.numPlanes;
            }
            else {
                b.bytesused = static_cast<uint32_t>(frameLen);
            }
            return xioctl(res.fd, VIDIOC_QBUF, &b) == 0 ? 0 : errno;
        };
        if (const int err = queueOutput(0, 1)) {
            fail("qbuf(output)", err);
            return;
        }
        ob[QStringLiteral("queuedBeforeStreamOn")] = 1;
        r[QStringLiteral("outputBuffers")] = ob;

        // 補送第二份 AU（buffer 1，和第一份相同的 IDR／CRA，不是 P 幀）。stage 記是為了哪一段送的；
        // 只送一次。driver 只配了一個 OUTPUT buffer 時不補送，這一段直接略過（記原因）。
        bool secondAuQueued = false;
        auto queueSecondAu = [&](const char* stage) -> bool {
            QJsonObject s;
            s[QStringLiteral("stage")] = QString::fromLatin1(stage);
            int err = 0;
            if (outCount < 2) {
                s[QStringLiteral("reason")] = QStringLiteral("driver allocated only 1 OUTPUT buffer");
            }
            else if ((err = queueOutput(1, 2)) != 0) {
                s[QStringLiteral("reason")] = QStringLiteral("qbuf(output) ") + errText(err);
            }
            else {
                secondAuQueued = true;
            }
            s[QStringLiteral("queued")] = secondAuQueued;
            r[QStringLiteral("secondAu")] = s;
            return secondAuQueued;
        };

        // 4. OUTPUT 開始串流，等 SOURCE_CHANGE（header 解析完成）。第一個 AU 帶齊 VPS/SPS/PPS，
        //    後面還接著 PPS 或 slice 的 start code，通常就夠；1 s 等不到才補送第二份再等 1 s。
        {
            int type = static_cast<int>(res.outType);
            if (xioctl(res.fd, VIDIOC_STREAMON, &type) != 0) {
                fail("streamon(output)", errno);
                return;
            }
        }
        const Clock::time_point tOut = Clock::now();
        bool sourceChange = false;
        bool eos = false;
        uint32_t changes = 0;
        double sourceChangeMs = -1;
        int sourceChangeWaitedMs = 0;
        bool sourceChangePollFailed = false;
        auto waitSourceChange = [&]() {
            const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kWaitMs);
            sourceChangeWaitedMs += kWaitMs;
            while (!sourceChange) {
                const int rev = waitFd(res.fd, static_cast<short>(POLLPRI), deadline, &errorWakeups);
                if (rev < 0) {
                    r[QStringLiteral("pollError")] = errText(errno);
                    sourceChangePollFailed = true;
                    break;
                }
                if (rev == 0) {
                    break;
                }
                if (dequeueEvents(res.fd, events, &sourceChange, &changes, &eos) == 0) {
                    shortBackoff();
                }
                if (sourceChange) {
                    sourceChangeMs = msSince(tOut);
                }
            }
        };
        waitSourceChange();
        bool sourceChangeNeededNextAu = false;
        if (!sourceChange && !sourceChangePollFailed && queueSecondAu("sourceChange")) {
            waitSourceChange();
            sourceChangeNeededNextAu = sourceChange;
        }
        {
            QJsonObject sc;
            sc[QStringLiteral("received")] = sourceChange;
            if (sourceChange) {
                sc[QStringLiteral("ms")] = sourceChangeMs;
                sc[QStringLiteral("changes")] = hex32(changes);
            }
            else {
                sc[QStringLiteral("waitedMs")] = sourceChangeWaitedMs;
            }
            r[QStringLiteral("sourceChange")] = sc;
            r[QStringLiteral("sourceChangeNeededNextAu")] = sourceChangeNeededNextAu;
            parts << (sourceChange ? QStringLiteral("sourceChange=") + msText(sourceChangeMs) +
                                         (sourceChangeNeededNextAu ? QStringLiteral("(nextAu)") : QString())
                                   : QStringLiteral("sourceChange=none(") + QString::number(sourceChangeWaitedMs) +
                                         QStringLiteral("ms)"));
        }
        if (!sourceChange) {
            // 沒等到事件仍照舊式流程繼續（部分 driver 不發 SOURCE_CHANGE，CAPTURE 就緒就解碼）；
            // 後面的結果要搭配這個旗標判讀
            r[QStringLiteral("captureSetupWithoutSourceChange")] = true;
        }

        // 5. CAPTURE 格式、可見區域、串流後的最小 capture buffer 數
        const CapFmt cf = getCaptureFormat(res.fd, mplane);
        if (!cf.ok) {
            fail("g_fmt(capture)", cf.err);
            return;
        }
        r[QStringLiteral("captureFormat")] = capFmtJson(cf);
        parts << QStringLiteral("capture=") + fourccStr(cf.fourcc) + QLatin1Char(' ') + QString::number(cf.width) +
                     QLatin1Char('x') + QString::number(cf.height);
        {
            QString text;
            r[QStringLiteral("compose")] = composeJson(res.fd, mplane, &text);
            parts << QStringLiteral("visible=") + text;
        }
        int minBuf = -1;
        {
            v4l2_control c{};
            c.id = kCidMinBuffersForCapture;
            if (xioctl(res.fd, VIDIOC_G_CTRL, &c) == 0) {
                minBuf = c.value;
                r[QStringLiteral("minBuffersForCapture")] = minBuf;
            }
            else {
                r[QStringLiteral("minBuffersForCapture")] = errText(errno);
            }
            parts << QStringLiteral("minBufCapture=") + (minBuf >= 0 ? QString::number(minBuf) : QStringLiteral("?"));
        }

        // 6. CAPTURE buffer：最小數量＋1（讀不到時用 iris 的靜態預設 4＋1）
        const int wantCap = std::clamp((minBuf > 0 ? minBuf : 4) + 1, 2, kMaxCaptureBuffers);
        v4l2_requestbuffers crb{};
        crb.count = static_cast<uint32_t>(wantCap);
        crb.type = res.capType;
        crb.memory = V4L2_MEMORY_MMAP;
        if (xioctl(res.fd, VIDIOC_REQBUFS, &crb) != 0) {
            fail("reqbufs(capture)", errno);
            return;
        }
        if (crb.count == 0) {
            fail("reqbufs(capture) allocated no buffers", ENOMEM);
            return;
        }
        const int capCount = std::min(static_cast<int>(crb.count), kMaxCaptureBuffers);
        {
            QJsonObject cb;
            cb[QStringLiteral("requested")] = wantCap;
            cb[QStringLiteral("allocated")] = static_cast<qint64>(crb.count);
            v4l2_buffer b{};
            v4l2_plane planes[VIDEO_MAX_PLANES] = {};
            b.index = 0;
            b.type = res.capType;
            b.memory = V4L2_MEMORY_MMAP;
            if (mplane) {
                b.m.planes = planes;
                b.length = cf.numPlanes;
            }
            if (xioctl(res.fd, VIDIOC_QUERYBUF, &b) == 0) {
                QJsonArray lengths;
                if (mplane) {
                    for (uint32_t p = 0; p < b.length && p < VIDEO_MAX_PLANES; ++p) {
                        lengths.append(static_cast<qint64>(planes[p].length));
                    }
                }
                else {
                    lengths.append(static_cast<qint64>(b.length));
                }
                cb[QStringLiteral("planeLengths")] = lengths;
            }
            else {
                cb[QStringLiteral("querybuf")] = errText(errno);
            }
            r[QStringLiteral("captureBuffers")] = cb;
        }

        // 7. EXPBUF：L1 與 DRM_PRIME 零拷貝的前提（capture buffer 能不能匯出成 dma-buf）
        if (expbuf) {
            QString text;
            r[QStringLiteral("expbuf")] = expbufJson(res.fd, res.capType, mplane ? cf.numPlanes : 1, &text);
            parts << QStringLiteral("expbuf=") + text;
        }

        // 8. 全部 CAPTURE buffer 入列，開始串流
        for (int i = 0; i < capCount; ++i) {
            v4l2_buffer b{};
            v4l2_plane planes[VIDEO_MAX_PLANES] = {};
            b.index = static_cast<uint32_t>(i);
            b.type = res.capType;
            b.memory = V4L2_MEMORY_MMAP;
            if (mplane) {
                b.m.planes = planes;
                b.length = cf.numPlanes;
            }
            if (xioctl(res.fd, VIDIOC_QBUF, &b) != 0) {
                fail("qbuf(capture)", errno);
                return;
            }
        }
        {
            int type = static_cast<int>(res.capType);
            if (xioctl(res.fd, VIDIOC_STREAMON, &type) != 0) {
                fail("streamon(capture)", errno);
                return;
            }
        }
        const Clock::time_point tCap = Clock::now();

        // 9. 等第一個有內容的 CAPTURE buffer，分三段（app 的 v4l2m2m 路徑一個 buffer 只放一個 AU、
        //    從不 drain，所以只有第一段出幀才代表沒有額外的幀延遲）：
        //    phase 0：只有第一份 AU，不 drain 等 1 s → outputAfter=immediate
        //    phase 1：補送第二份 AU 再等 1 s → nextAu（要看到下一個 AU 才吐幀：串流時每幀至少多等
        //             一個幀間隔）。第二份是和第一份相同的 IDR／CRA、不是 P 幀，被第二張 IRAP 擠出
        //             DPB 的情況也算在這裡，所以這是樂觀的下限
        //    phase 2：DEC_CMD_STOP 再等 1 s → drain
        //    第 4 步為了 SOURCE_CHANGE 已經補送過第二份時，phase 0 出幀只能記 nextAu，phase 1 略過；
        //    補送不出去（只有一個 OUTPUT buffer 等）時 phase 1 也略過，直接 drain。
        QJsonObject ff;
        double firstFrameMs = -1;
        bool gotLast = false;
        bool drainIssued = false;
        bool abort = false;
        uint32_t lateChanges = 0;
        const char* outputAfter = "none";
        for (int phase = 0; phase < 3 && !gotFrame && !gotLast && !abort; ++phase) {
            if (phase == 1) {
                if (secondAuQueued || !queueSecondAu("firstFrame")) {
                    continue;
                }
            }
            else if (phase == 2) {
                v4l2_decoder_cmd dc{};
                dc.cmd = V4L2_DEC_CMD_STOP;
                if (xioctl(res.fd, VIDIOC_DECODER_CMD, &dc) != 0) {
                    ff[QStringLiteral("drain")] = errText(errno);  // drainIssued 維持 false
                    break;
                }
                drainIssued = true;
                ff[QStringLiteral("drain")] = QStringLiteral("issued");
            }
            const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kWaitMs);
            while (!gotFrame && !gotLast) {
                const int rev = waitFd(res.fd, static_cast<short>(POLLIN | POLLPRI), deadline, &errorWakeups);
                if (rev < 0) {
                    r[QStringLiteral("pollError")] = errText(errno);
                    abort = true;
                    break;
                }
                if (rev == 0) {
                    break;
                }
                const bool gotEvents =
                    (rev & POLLPRI) && dequeueEvents(res.fd, events, &lateSourceChange, &lateChanges, &eos) > 0;
                if (!(rev & POLLIN)) {
                    if (!gotEvents) {
                        shortBackoff();
                    }
                    continue;
                }
                v4l2_buffer b{};
                v4l2_plane planes[VIDEO_MAX_PLANES] = {};
                b.type = res.capType;
                b.memory = V4L2_MEMORY_MMAP;
                if (mplane) {
                    b.m.planes = planes;
                    b.length = VIDEO_MAX_PLANES;
                }
                if (xioctl(res.fd, VIDIOC_DQBUF, &b) != 0) {
                    const int err = errno;
                    if (err == EAGAIN) {
                        shortBackoff();  // POLLIN 亮著卻取不到：不空轉
                        continue;
                    }
                    if (err == EPIPE) {
                        gotLast = true;  // LAST buffer 之後再 DQBUF 就是 EPIPE
                        ff[QStringLiteral("dqbuf")] = QStringLiteral("EPIPE(after last buffer)");
                        break;
                    }
                    ff[QStringLiteral("dqbuf")] = errText(err);
                    abort = true;
                    break;
                }
                qint64 bytes = 0;
                QJsonArray perPlane;
                if (mplane) {
                    for (uint32_t p = 0; p < b.length && p < VIDEO_MAX_PLANES; ++p) {
                        bytes += planes[p].bytesused;
                        perPlane.append(static_cast<qint64>(planes[p].bytesused));
                    }
                }
                else {
                    bytes = b.bytesused;
                    perPlane.append(static_cast<qint64>(b.bytesused));
                }
                const bool errFlag = (b.flags & V4L2_BUF_FLAG_ERROR) != 0;
                const bool last = (b.flags & V4L2_BUF_FLAG_LAST) != 0;
                if (bytes == 0 && !errFlag) {
                    // 規格：bytesused=0 的 buffer 不含影格，忽略。帶 LAST 的通常是 drain 的結尾，
                    // 但也可能是解析度變更（CAPTURE 用 placeholder 格式串流後才到 SOURCE_CHANGE）
                    if (last) {
                        gotLast = true;
                        ff[QStringLiteral("emptyLastBuffer")] = true;
                    }
                    continue;
                }
                gotFrame = true;
                frameErrorFlag = errFlag;
                firstFrameMs = msSince(tCap);
                ff[QStringLiteral("ms")] = firstFrameMs;
                ff[QStringLiteral("index")] = static_cast<qint64>(b.index);
                ff[QStringLiteral("bytesused")] = bytes;
                ff[QStringLiteral("bytesusedPerPlane")] = perPlane;
                ff[QStringLiteral("flags")] = QJsonArray::fromStringList(flagNames(b.flags, kBufFlagNames));
                ff[QStringLiteral("errorFlag")] = errFlag;
                ff[QStringLiteral("last")] = last;
                ff[QStringLiteral("sequence")] = static_cast<qint64>(b.sequence);
                ff[QStringLiteral("timestampUs")] =
                    static_cast<qint64>(b.timestamp.tv_sec) * 1000000 + static_cast<qint64>(b.timestamp.tv_usec);
                ff[QStringLiteral("expectedTimestampsUs")] = QJsonArray{1000, 2000};
            }
            if (gotFrame) {
                outputAfter = phase == 2 ? "drain" : secondAuQueued ? "nextAu" : "immediate";
            }
        }
        ff[QStringLiteral("received")] = gotFrame;
        ff[QStringLiteral("outputAfter")] = QString::fromLatin1(outputAfter);
        ff[QStringLiteral("drainIssued")] = drainIssued;
        // 相容舊欄位：immediate 與 nextAu 都是 true；只有 outputAfter=immediate 代表沒有額外的幀延遲
        ff[QStringLiteral("withoutDrain")] = gotFrame && !drainIssued;
        if (lateSourceChange) {
            ff[QStringLiteral("sourceChangeDuringCapture")] = true;
            ff[QStringLiteral("sourceChangeChanges")] = hex32(lateChanges);
        }
        if (eos) {
            ff[QStringLiteral("eosEvent")] = true;
        }
        r[QStringLiteral("firstFrame")] = ff;
        if (gotFrame) {
            parts << QStringLiteral("firstFrame=") + msText(firstFrameMs) +
                         QStringLiteral(" out=") + QString::fromLatin1(outputAfter) +
                         QStringLiteral(" errFlag=") + (frameErrorFlag ? QStringLiteral("1") : QStringLiteral("0"));
        }
        else {
            // 沒送 DEC_CMD_STOP 卻拿到 LAST 時不能說 drained：多半是解析度變更，CAPTURE 要重新配置
            QString why;
            if (gotLast && drainIssued) {
                why = QStringLiteral("(drained, no frame)");
            }
            else if (gotLast && lateSourceChange) {
                why = QStringLiteral("(source change, capture needs reconfig)");
            }
            else if (gotLast) {
                why = QStringLiteral("(LAST without drain)");
            }
            else if (lateSourceChange) {
                why = QStringLiteral("(source change during capture)");
            }
            parts << QStringLiteral("firstFrame=none") + why;
        }
    }();

    if (!events.isEmpty()) {
        r[QStringLiteral("events")] = events;
    }
    if (errorWakeups > 0) {
        r[QStringLiteral("pollErrorWakeups")] = errorWakeups;
    }

    QString result;
    if (!failure.isEmpty()) {
        result = failure;
    }
    else if (!gotFrame) {
        result = lateSourceChange ? QStringLiteral("source-change-during-capture") : QStringLiteral("no-frame");
    }
    else {
        result = frameErrorFlag ? QStringLiteral("frame-with-error-flag") : QStringLiteral("ok");
    }
    r[QStringLiteral("result")] = result;

    *lineOut = QStringLiteral("header-test codec=") + QString::fromLatin1(codec.name) + QLatin1Char(' ') +
               QString::number(kTestFrameWidth) + QLatin1Char('x') + QString::number(kTestFrameHeight) +
               (parts.isEmpty() ? QString() : QLatin1Char(' ') + parts.join(QLatin1Char(' '))) +
               QStringLiteral(" result=") + result;
    return r;
    // res 在這裡解構：STREAMOFF → munmap → REQBUFS(0) → close
}

// 取 app 的測試幀；拿不到時 whyNot 說明原因
bool fetchTestFrame(const CodecInfo& codec, const uint8_t** data, size_t* length, QString* whyNot)
{
#ifdef HAVE_FFMPEG
    int videoFormat = 0;
    if (codec.fourcc == fcc('H', '2', '6', '4')) {
        videoFormat = VIDEO_FORMAT_H264;
    }
    else if (codec.fourcc == fcc('H', 'E', 'V', 'C')) {
        videoFormat = VIDEO_FORMAT_H265;
    }
    else {
        *whyNot = QStringLiteral("no test frame for this codec");
        return false;
    }
    if (!FFmpegVideoDecoder::getTestFrameData(videoFormat, data, length) || *data == nullptr || *length == 0) {
        *whyNot = QStringLiteral("no test frame for this codec");
        return false;
    }
    return true;
#else
    Q_UNUSED(codec);
    Q_UNUSED(data);
    Q_UNUSED(length);
    *whyNot = QStringLiteral("header-test unavailable (built without FFmpeg)");
    return false;
#endif
}

QJsonArray runHeaderTests(const QString& path, bool mplane, const std::vector<FmtDesc>& outFmts,
                          const V4l2Caps::ProbeOptions& options)
{
    QJsonArray tests;
    QStringList wanted;
    for (const QString& c : options.headerCodecs) {
        wanted << c.trimmed().toLower();
    }
    const bool implicitAll = wanted.isEmpty();
    if (implicitAll) {
        // 空 = 該裝置支援、而且有測試幀的 codec
        for (const CodecInfo& c : kCodecs) {
            if (c.headerTestKey != nullptr) {
                wanted << QString::fromLatin1(c.headerTestKey);
            }
        }
    }

    for (const QString& key : wanted) {
        QJsonObject t;
        t[QStringLiteral("codec")] = key;
        const CodecInfo* codec = findCodecByHeaderKey(key);
        QString skip;
        if (codec == nullptr) {
            skip = QStringLiteral("unknown codec (expected h264 or hevc)");
        }
        else if (std::none_of(outFmts.begin(), outFmts.end(), [codec](const FmtDesc& f) { return f.fourcc == codec->fourcc; })) {
            skip = QStringLiteral("device has no ") + fourccStr(codec->fourcc) + QStringLiteral(" OUTPUT format");
        }
        const uint8_t* data = nullptr;
        size_t length = 0;
        if (skip.isEmpty()) {
            QString why;
            if (!fetchTestFrame(*codec, &data, &length, &why)) {
                skip = why.isEmpty() ? QStringLiteral("no test frame for this codec") : why;
            }
        }
        if (!skip.isEmpty()) {
            t[QStringLiteral("skipped")] = skip;
            tests.append(t);
            ProbeUtil::printLine(kTag, "dev=%s header-test codec=%s skipped: %s", qUtf8Printable(path),
                                 qUtf8Printable(key), qUtf8Printable(skip));
            continue;
        }
        QString line;
        tests.append(runHeaderTest(path, mplane, *codec, data, length, options.expbuf, &line));
        ProbeUtil::printLine(kTag, "dev=%s %s", qUtf8Printable(path), qUtf8Printable(line));
    }
    return tests;
}

// ── 單一裝置的完整探測 ──

QJsonObject probeDeviceImpl(const QString& path, const V4l2Caps::ProbeOptions& options)
{
    const Clock::time_point t0 = Clock::now();
    QJsonObject dev;
    dev[QStringLiteral("supported")] = true;
    dev[QStringLiteral("path")] = path;
    const QString sysName = sysfsNameForPath(path);
    if (!sysName.isEmpty()) {
        dev[QStringLiteral("sysName")] = sysName;
    }
    dev[QStringLiteral("m2m")] = false;
    dev[QStringLiteral("isDecoder")] = false;

    QString statText;
    dev[QStringLiteral("node")] = nodeStatJson(path, &statText);

    const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        const int err = errno;
        QJsonObject openObj = errJson(err);
        openObj[QStringLiteral("ok")] = false;
        const QString hint = openHint(err);
        if (!hint.isEmpty()) {
            openObj[QStringLiteral("hint")] = hint;
        }
        dev[QStringLiteral("open")] = openObj;
        QString groupsText;
        dev[QStringLiteral("process")] = processIdentityJson(&groupsText);
        dev[QStringLiteral("role")] = QStringLiteral("unavailable");
        ProbeUtil::printLine(kTag, "dev=%s open=failed %s %s groups=[%s]%s%s", qUtf8Printable(path),
                             qUtf8Printable(errText(err)), qUtf8Printable(statText), qUtf8Printable(groupsText),
                             hint.isEmpty() ? "" : " hint=", qUtf8Printable(hint));
        dev[QStringLiteral("probeMs")] = msSince(t0);
        return dev;
    }

    bool isDecoder = false;
    bool mplane = false;
    std::vector<FmtDesc> outFmts;
    {
        FdCloser closer{fd};
        {
            QJsonObject openObj;
            openObj[QStringLiteral("ok")] = true;
            dev[QStringLiteral("open")] = openObj;
        }

        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
            const int err = errno;
            dev[QStringLiteral("querycap")] = errJson(err);
            dev[QStringLiteral("role")] = QStringLiteral("not-v4l2");
            ProbeUtil::printLine(kTag, "dev=%s open=ok querycap=%s (not a V4L2 device?) %s", qUtf8Printable(path),
                                 qUtf8Printable(errText(err)), qUtf8Printable(statText));
            dev[QStringLiteral("probeMs")] = msSince(t0);
            return dev;
        }

        V4l2Caps::DeviceSummary s;
        bool legacy = false;
        applyCaps(s, cap, &legacy);
        {
            QJsonObject qc;
            qc[QStringLiteral("driver")] = s.driver;
            qc[QStringLiteral("card")] = s.card;
            qc[QStringLiteral("busInfo")] = s.busInfo;
            qc[QStringLiteral("version")] = kernelVersionStr(cap.version);
            qc[QStringLiteral("capabilities")] = hex32(cap.capabilities);
            qc[QStringLiteral("deviceCaps")] = hex32(s.deviceCaps);
            qc[QStringLiteral("deviceCapNames")] = QJsonArray::fromStringList(flagNames(s.deviceCaps, kCapNames));
            dev[QStringLiteral("querycap")] = qc;
        }
        dev[QStringLiteral("m2m")] = s.m2m;
        dev[QStringLiteral("m2mMplane")] = s.m2mMplane;
        dev[QStringLiteral("m2mLegacyCaps")] = legacy;

        const QString idText = QStringLiteral("driver=") + s.driver + QStringLiteral(" card=\"") + s.card +
                               QStringLiteral("\" bus=") + s.busInfo + QStringLiteral(" version=") +
                               kernelVersionStr(cap.version) + QStringLiteral(" caps=") + hex32(s.deviceCaps);

        if (!s.m2m) {
            // 不是 m2m（攝影機等）：只做 QUERYCAP，不碰格式，避免打擾正在使用它的行程
            dev[QStringLiteral("role")] = QStringLiteral("not-m2m");
            ProbeUtil::printLine(kTag, "dev=%s open=ok %s m2m=0 role=not-m2m", qUtf8Printable(path), qUtf8Printable(idText));
            dev[QStringLiteral("probeMs")] = msSince(t0);
            return dev;
        }

        mplane = s.m2mMplane;
        const uint32_t outType = mplane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT;
        const uint32_t capType = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        dev[QStringLiteral("bufferTypes")] = mplane ? QStringLiteral("multi-planar") : QStringLiteral("single-planar");

        int outErr = 0;
        int capErr = 0;
        outFmts = enumFormats(fd, outType, &outErr);
        const std::vector<FmtDesc> capFmtsDefault = enumFormats(fd, capType, &capErr);

        // decoder＝OUTPUT 有 compressed 的視訊格式（JPEG 等非視訊格式另記，不算）。kind 只看 kCodecs
        // 表內的格式；表外但計入的格式（例如沒查證過的 AV1 fourcc）不預設成 stateful。
        bool anyCompressedCap = false;
        bool anyStateful = false;
        bool anyStateless = false;
        QStringList codedNames;
        QStringList otherCoded;
        QStringList statefulVideo;
        for (const FmtDesc& f : outFmts) {
            if (!(f.flags & V4L2_FMT_FLAG_COMPRESSED)) {
                continue;
            }
            if (isNonVideoCoded(f.fourcc)) {
                otherCoded << fourccStr(f.fourcc);
                continue;
            }
            if (const CodecInfo* c = findCodec(f.fourcc)) {
                if (c->stateless) {
                    anyStateless = true;
                }
                else {
                    anyStateful = true;
                    if (isStatefulVideoCodec(*c) && !statefulVideo.contains(QLatin1String(c->name))) {
                        statefulVideo << QString::fromLatin1(c->name);
                    }
                }
            }
            codedNames << fourccStr(f.fourcc);
        }
        for (const FmtDesc& f : capFmtsDefault) {
            if (f.flags & V4L2_FMT_FLAG_COMPRESSED) {
                anyCompressedCap = true;
            }
        }
        isDecoder = !codedNames.isEmpty();
        const QString role = isDecoder              ? QStringLiteral("decoder")
                           : !otherCoded.isEmpty()  ? QStringLiteral("non-video-decoder")
                           : anyCompressedCap       ? QStringLiteral("encoder")
                                                    : QStringLiteral("converter");
        const QString kind = anyStateful && anyStateless ? QStringLiteral("mixed")
                           : anyStateless                ? QStringLiteral("stateless")
                           : anyStateful                 ? QStringLiteral("stateful")
                                                         : QStringLiteral("unknown");
        dev[QStringLiteral("role")] = role;
        dev[QStringLiteral("isDecoder")] = isDecoder;
        if (isDecoder) {
            dev[QStringLiteral("decoderKind")] = kind;
            // app 的 v4l2m2m 路徑能直接用的（H264/HEVC/AV1/VP9 且非 stateless）
            dev[QStringLiteral("statefulVideoCodecs")] = QJsonArray::fromStringList(statefulVideo);
        }
        dev[QStringLiteral("codedFormats")] = QJsonArray::fromStringList(codedNames);
        if (!otherCoded.isEmpty()) {
            dev[QStringLiteral("otherCodedFormats")] = QJsonArray::fromStringList(otherCoded);
        }
        if (outFmts.empty() && outErr != 0) {
            dev[QStringLiteral("outputEnumError")] = errText(outErr);
        }
        {
            QJsonArray arr;
            for (const FmtDesc& f : capFmtsDefault) {
                arr.append(fmtJson(f));
            }
            dev[QStringLiteral("captureFormatsDefault")] = arr;
            if (capFmtsDefault.empty() && capErr != 0) {
                dev[QStringLiteral("captureEnumError")] = errText(capErr);
            }
        }

        const QString kindText = isDecoder ? QStringLiteral(" kind=") + kind : QString();
        const QString otherText = otherCoded.isEmpty()
                                      ? QString()
                                      : QStringLiteral(" otherCoded=[") + otherCoded.join(QLatin1Char(',')) + QLatin1Char(']');
        ProbeUtil::printLine(kTag, "dev=%s open=ok %s m2m=1 mplane=%d legacyCaps=%d role=%s%s coded=[%s]%s",
                             qUtf8Printable(path), qUtf8Printable(idText), mplane ? 1 : 0, legacy ? 1 : 0,
                             qUtf8Printable(role), qUtf8Printable(kindText),
                             qUtf8Printable(codedNames.join(QLatin1Char(','))), qUtf8Printable(otherText));

        // OUTPUT 格式；decoder 的每個 coded 視訊格式另外 S_FMT 後查 CAPTURE 格式與控制項
        QJsonArray outArr;
        for (const FmtDesc& f : outFmts) {
            QJsonObject fo = fmtJson(f);
            if (!isDecoder || !(f.flags & V4L2_FMT_FLAG_COMPRESSED) || isNonVideoCoded(f.fourcc)) {
                outArr.append(fo);
                continue;
            }
            const CodecInfo* codec = findCodec(f.fourcc);

            QString sizeText;
            fo[QStringLiteral("frameSizes")] = frameSizesJson(fd, f.fourcc, &sizeText);
            QString line = QStringLiteral("dev=") + path + QStringLiteral(" coded=") + fourccStr(f.fourcc) +
                           (codec != nullptr ? QLatin1Char('(') + QString::fromLatin1(codec->name) + QLatin1Char(')') : QString()) +
                           QStringLiteral(" flags=") + flagText(f.flags, kFmtFlagNames) + QStringLiteral(" size=") + sizeText;

            const OutFmtResult of = setOutputFormat(fd, mplane, f.fourcc, kEnumWidth, kEnumHeight);
            fo[QStringLiteral("sFmt")] = outFmtJson(of);
            if (!of.ok || of.fourcc != f.fourcc) {
                // 設不上去（或被換成別的格式）時，CAPTURE 列舉反映的是上一個格式，不能記
                line += of.ok ? QStringLiteral(" s_fmt=substituted(") + fourccStr(of.fourcc) + QLatin1Char(')')
                              : QStringLiteral(" s_fmt=") + errText(of.err);
                ProbeUtil::printLine(kTag, "%s", qUtf8Printable(line));
                outArr.append(fo);
                continue;
            }

            int cErr = 0;
            const std::vector<FmtDesc> capFmts = enumFormats(fd, capType, &cErr);
            QJsonArray capArr;
            QStringList capNames;
            for (const FmtDesc& c : capFmts) {
                QJsonObject co = fmtJson(c);
                QString t;
                co[QStringLiteral("frameSizes")] = frameSizesJson(fd, c.fourcc, &t);
                capArr.append(co);
                capNames << fourccStr(c.fourcc);
            }
            fo[QStringLiteral("captureFormats")] = capArr;
            if (capFmts.empty() && cErr != 0) {
                fo[QStringLiteral("captureEnumError")] = errText(cErr);
            }
            line += QStringLiteral(" capture=[") + capNames.join(QLatin1Char(',')) + QLatin1Char(']');

            int ctrlErr = 0;
            const std::vector<CtrlInfo> ctrls = walkControls(fd, &ctrlErr);
            {
                QJsonArray ctrlArr;
                for (const CtrlInfo& c : ctrls) {
                    ctrlArr.append(ctrlJson(c));
                }
                fo[QStringLiteral("controls")] = ctrlArr;
                if (ctrls.empty() && ctrlErr != 0) {
                    fo[QStringLiteral("controlsError")] = errText(ctrlErr);
                }
            }
            if (codec != nullptr) {
                QString fragment;
                fo[QStringLiteral("codecSummary")] = codecSummaryJson(*codec, ctrls, &fragment);
                line += fragment;
            }
            const CtrlInfo* mb = findCtrl(ctrls, kCidMinBuffersForCapture);
            if (mb != nullptr && mb->hasValue) {
                fo[QStringLiteral("minBuffersForCaptureStatic")] = mb->value;
                line += QStringLiteral(" minBufCapture(static)=") + QString::number(mb->value);
            }
            else {
                line += QStringLiteral(" minBufCapture(static)=?");
            }
            const CtrlInfo* mbo = findCtrl(ctrls, kCidMinBuffersForOutput);
            if (mbo != nullptr && mbo->hasValue) {
                fo[QStringLiteral("minBuffersForOutputStatic")] = mbo->value;
            }
            ProbeUtil::printLine(kTag, "%s", qUtf8Printable(line));
            outArr.append(fo);
        }
        dev[QStringLiteral("outputFormats")] = outArr;

        if (isDecoder) {
            // queue 能力：只對 decoder 查（理由見 queueCapsJson）。舊式旗標判定的節點也跳過：
            // exclusive_caps=0 的 v4l2loopback 就是被這條判成 m2m 的。這個 fd 還沒配置過 buffer，
            // 放在 S_FMT 之後，對 vb2 的 REQBUFS(0) 仍然沒有副作用。
            QString qOutText;
            QString qCapText;
            if (legacy) {
                qOutText = QStringLiteral("skipped(legacy-caps)");
                qCapText = qOutText;
                dev[QStringLiteral("queueCaps")] = QStringLiteral("skipped (legacy caps)");
            }
            else {
                QJsonObject queueCaps;
                queueCaps[QStringLiteral("output")] = queueCapsJson(fd, outType, &qOutText);
                queueCaps[QStringLiteral("capture")] = queueCapsJson(fd, capType, &qCapText);
                dev[QStringLiteral("queueCaps")] = queueCaps;
            }

            const QString stopText = tryDecoderCmd(fd, V4L2_DEC_CMD_STOP);
            const QString startText = tryDecoderCmd(fd, V4L2_DEC_CMD_START);
            QJsonObject dc;
            dc[QStringLiteral("stop")] = stopText;
            dc[QStringLiteral("start")] = startText;
            dev[QStringLiteral("tryDecoderCmd")] = dc;

            QString pOut;
            QString pCap;
            QJsonObject parm;
            parm[QStringLiteral("output")] = parmJson(fd, outType, true, &pOut);
            parm[QStringLiteral("capture")] = parmJson(fd, capType, false, &pCap);
            dev[QStringLiteral("parm")] = parm;

            ProbeUtil::printLine(kTag, "dev=%s decoderCmd stop=%s start=%s g_parm output=%s capture=%s queueCaps output=%s capture=%s",
                                 qUtf8Printable(path), qUtf8Printable(stopText), qUtf8Printable(startText),
                                 qUtf8Printable(pOut), qUtf8Printable(pCap), qUtf8Printable(qOutText),
                                 qUtf8Printable(qCapText));
        }
        // closer 在這裡關掉列舉用的 fd；header-test 另開一個乾淨的 context
    }

    if (isDecoder && options.headerTest) {
        dev[QStringLiteral("headerTest")] = runHeaderTests(path, mplane, outFmts, options);
    }
    dev[QStringLiteral("probeMs")] = msSince(t0);
    return dev;
}

} // namespace

namespace V4l2Caps {

bool isSupported()
{
    return true;
}

QList<DeviceSummary> enumerateSysfsOnly()
{
    // 只收 video*（v4l-subdev／media 等不是 decoder 節點），與 enumerate() 的 /dev/video* 對應
    QList<DeviceSummary> out;
    const QStringList names = listNumberedEntries("/sys/class/video4linux", "video");
    for (const QString& name : names) {
        DeviceSummary s;
        s.path = QStringLiteral("/dev/") + name;
        s.sysName = readSysfsName(name);
        out.append(s);
    }
    return out;
}

QList<DeviceSummary> enumerate()
{
    QList<DeviceSummary> out;
    const QStringList names = listNumberedEntries("/dev", "video");
    for (const QString& name : names) {
        DeviceSummary s;
        s.path = QStringLiteral("/dev/") + name;
        s.sysName = readSysfsName(name);
        const int fd = ::open(QFile::encodeName(s.path).constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            s.openErrno = errno;
            out.append(s);
            continue;
        }
        v4l2_capability cap{};
        if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            applyCaps(s, cap, nullptr);
        }
        ::close(fd);
        out.append(s);
    }
    return out;
}

QJsonObject probeDevice(const QString& path, const ProbeOptions& options)
{
    return probeDeviceImpl(path, options);
}

} // namespace V4l2Caps

#else // !VIPLE_HAVE_V4L2

// 非 Linux（或沒有 <linux/videodev2.h>）的建置：一律回「不支援」
namespace V4l2Caps {

bool isSupported()
{
    return false;
}

QList<DeviceSummary> enumerateSysfsOnly()
{
    return {};
}

QList<DeviceSummary> enumerate()
{
    return {};
}

QJsonObject probeDevice(const QString& path, const ProbeOptions& options)
{
    Q_UNUSED(path);
    Q_UNUSED(options);
    QJsonObject o;
    o[QStringLiteral("supported")] = false;
    return o;
}

} // namespace V4l2Caps

#endif // VIPLE_HAVE_V4L2

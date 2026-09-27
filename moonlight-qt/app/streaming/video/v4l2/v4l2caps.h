// VipleStream §SF-PROBE（M2a）— V4L2 能力列舉（純 ioctl，不依賴 FFmpeg）
//
// 給 `v4l2-probe`（PoC-0／PoC-3 的 Day-1 探測）與 `[VIPLE-SF-ENV]` 共用；日後 L1 的自寫
// V4l2Decoder 也用這裡的列舉。Frame 上跑的是 upstream iris 還是 Qualcomm 下游驅動目前
// UNVERIFIED，所以這裡絕不寫死 driver 名稱，只看 capability 旗標。
//
// 一律編譯：只有 `Q_OS_LINUX` 且有 <linux/videodev2.h> 時是真實作，其他平台 isSupported()
// 回 false、列舉回空清單、probeDevice() 回只含 {"supported": false} 的物件。

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

namespace V4l2Caps {

struct DeviceSummary {
    QString path;              // /dev/videoN
    QString sysName;           // /sys/class/video4linux/videoN/name（讀不到時為空）
    int openErrno = 0;         // 0 = open 成功；否則是 open(2) 的 errno
    QString driver;            // VIDIOC_QUERYCAP 的 driver
    QString card;
    QString busInfo;
    uint32_t deviceCaps = 0;   // V4L2_CAP_DEVICE_CAPS 有設時用 device_caps，否則用 capabilities
    bool m2m = false;          // V4L2_CAP_VIDEO_M2M 或 V4L2_CAP_VIDEO_M2M_MPLANE；舊式 driver 只宣告
                               // CAPTURE+OUTPUT(+STREAMING) 組合時也算（照 FFmpeg v4l2_m2m.c 的判斷）
    bool m2mMplane = false;    // V4L2_CAP_VIDEO_M2M_MPLANE，或舊式的 CAPTURE_MPLANE+OUTPUT_MPLANE；兩種都有時優先
};

// 本建置有真實作（Linux + videodev2.h）。
bool isSupported();

// 只讀 sysfs（/sys/class/video4linux/*/name），不開裝置。給 SfEnv::logOnce() 用（便宜）。
// 回傳的 DeviceSummary 只有 path 與 sysName 有值。只收 videoN（和 enumerate() 的 /dev/video*
// 對應；v4l-subdev 等不是 decoder 節點），依數字排序。
QList<DeviceSummary> enumerateSysfsOnly();

// 列出 /dev/video*（依數字排序），每個節點 open(O_RDWR|O_NONBLOCK|O_CLOEXEC) + QUERYCAP 後關閉。
QList<DeviceSummary> enumerate();

struct ProbeOptions {
    // header-test 會真的佔用 VPU：先只送一份 720p 測試 AU、等 SOURCE_CHANGE（等不到才補送第二份，
    // JSON 記 sourceChangeNeededNextAu）、讀 CAPTURE 格式與串流後的 MIN_BUFFERS_FOR_CAPTURE，再分三段
    // 等首幀：只有一個 AU／補送下一個 AU／DEC_CMD_STOP，結果記在 firstFrame.outputAfter
    // （immediate｜nextAu｜drain｜none；只有 immediate 代表沒有額外的幀延遲）。每個 codec 最壞約 4 s。
    // expbuf 另測 capture buffer 的 EXPBUF。
    bool headerTest = false;
    QStringList headerCodecs;  // "h264"、"hevc"；空 = 所有該裝置支援、而且有測試幀的 codec
    bool expbuf = false;
};

// 完整探測一個裝置：QUERYCAP、OUTPUT/CAPTURE 的 ENUM_FMT 與 ENUM_FRAMESIZES、控制項
// （profile/level/tier menu、MIN_BUFFERS_FOR_CAPTURE 靜態值）、TRY_DECODER_CMD、G_PARM，
// 以及 options 指定的 header-test。開檔失敗時記 errno、stat 的 mode/gid 與本行程的
// getgroups()（判斷是權限還是沙箱問題）。每一行重點同時以 `[VIPLE-V4L2-PROBE]` 記 log。
// decoder（JSON 的 isDecoder）＝m2m 且 OUTPUT 至少有一個 compressed 的視訊格式；JPEG／MJPEG／DV／
// MPEG 容器等非視訊格式記在 otherCodedFormats，不算（role=non-video-decoder）。decoder 另記
// statefulVideoCodecs（app 的 v4l2m2m 路徑能用的 H264/HEVC/AV1/VP9）。會動到 queue 狀態的
// REQBUFS(0)（queueCaps）只對 decoder、而且不是舊式旗標判定的節點做。
QJsonObject probeDevice(const QString& path, const ProbeOptions& options);

} // namespace V4l2Caps

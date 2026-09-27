// VipleStream §SF-PROBE（M2a）— `viplestream v4l2-probe`：`[VIPLE-V4L2-PROBE]`
//
// PoC-0／PoC-3 的 Day-1 探測：沙箱內看不看得到 /dev/video*、有沒有 stateful m2m decoder、
// 各 codec 的 profile／level 上限、CAPTURE 格式（NV12 或 UBWC 壓縮格式）、最小 capture
// buffer 數、能否 EXPBUF。G-α 的替代路徑條件（「沒有 /dev/video*」）直接看這支的結果。
//
// 結束碼：0 至少一個 m2m decoder；13 沒有可用的 m2m decoder（`no m2m decoder visible`）；
// 10 非 Linux 建置；其餘見 cli/probeutil.h。

#pragma once

#include <QString>

struct V4l2ProbeOptions {
    QString device;       // 空 = 探測所有 /dev/video*
    QString headerTest;   // "" 不做；"h264"、"hevc"、"all"
    bool expbuf = false;  // header-test 時另測 VIDIOC_EXPBUF
    QString jsonPath;     // 空 = ProbeUtil::defaultJsonPath("v4l2-probe")
};

int runV4l2Probe(const V4l2ProbeOptions& options);

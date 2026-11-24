#pragma once

// §QUEST-VR：Quest 的 OpenXR 與手機模擬共用的姿態來源。這一層不建立 OpenXR session。

#include <Limelight.h>

#include <cstdint>

struct QxrPoseSource {
    virtual ~QxrPoseSource() = default;
    // 填一筆完整的 VIPLE_VR_TRACKING（頭、兩手、輸入、旗標）。sampleId 與時間欄位由呼叫端填。
    // 回 false＝這一拍跳過，呼叫端不占用 sampleId。
    virtual bool sample(VIPLE_VR_TRACKING* out, uint64_t nowUs) = 0;
};

/**
 * @file streaming/vr/vrsynthetic.h
 * @brief VipleStream 2.0 §VR：決定性的合成 pose（--vr-emulate 用，取代 XR runtime）。
 *
 * 同一個時間 t 一定產生同一組 pose，server 回聲時可以逐幀比對。
 */
#pragma once

#include <Limelight.h>

#include <QString>

enum class VrSyntheticMotion {
    Still = 0,  // 靜止：HMD 在 (0, 1.6, 0)、面向 -Z
    Sine  = 1,  // 小幅位移加左右／上下擺頭（預設）
    Yaw30 = 2,  // 以 30°/s 持續轉頭
    // 2026-10-05（dev）：host 的 vr_probe --mode predict 用。頭先繞本地 X 傾 30°、再以 30°/s 繞世界 Y 轉，x 方向 ±20 cm
    // 平移；右手控制器的姿態與頭完全相同（位置偏右）。頭水平時世界與本地的 yaw 軸相同，分不出 SteamVR 怎麼解讀角速度。
    Tilt30Yaw = 3,
    // 2026-10-07（dev）：接近實際遊玩（桌球）量級的頭部運動——左右擺頭 ±30° @1.2 Hz（峰值約 226°/s）、上下 ±15° @1.7 Hz、
    // 位移 x ±15 cm @1 Hz、y ±5 cm @1.3 Hz。Sine 的轉速只有約 31°/s，對不上真人在玩時才出現的現象
    Fast = 4,
};

bool vrSyntheticMotionFromString(const QString& name, VrSyntheticMotion* out);
const char* vrSyntheticMotionName(VrSyntheticMotion motion);

// 填 out 的 flags、pose[]、input[]、gaze；version／sampleId／時間欄位由呼叫端填
void vrSyntheticFill(VrSyntheticMotion motion, double tSec, VIPLE_VR_TRACKING* out);

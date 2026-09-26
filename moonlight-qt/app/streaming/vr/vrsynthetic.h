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
};

bool vrSyntheticMotionFromString(const QString& name, VrSyntheticMotion* out);
const char* vrSyntheticMotionName(VrSyntheticMotion motion);

// 填 out 的 flags、pose[]、input[]、gaze；version／sampleId／時間欄位由呼叫端填
void vrSyntheticFill(VrSyntheticMotion motion, double tSec, VIPLE_VR_TRACKING* out);

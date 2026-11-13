// VipleStream §XR-PADMODE（2026-11-13）— XR 桌面（2D 串流）的手把模式用的純邏輯：長按偵測與方向盤角度。
//
// 方向盤的算法與 host 的 Sunshine/src/vr/vr_padmode_map.h（§VR-WHEEL，VR 串流時在 host 端算）是同一份：
// 兩手握著一個看不見的方向盤，轉動的角度取代左搖桿 X。改其中一邊時另一邊要一起改。
// 純標頭、不依賴 OpenXR，可以單獨編譯測試。

#pragma once

#include <cmath>
#include <cstdint>

namespace xrwheel {

constexpr float kPi = 3.14159265358979f;

struct Wheel {
    float axisX = 0.0f;  // 方向盤平面的水平軸（單位向量，x／z）；0／0＝還沒建立
    float axisZ = 0.0f;
    float angle = 0.0f;  // 展開後的角度（弧度，右轉為正）
    bool valid = false;

    static constexpr float kMinSpanM = 0.12f;     // 兩手距離小於這個值（手疊在一起）不更新
    static constexpr float kFollow = 0.02f;       // 擺正時水平軸追蹤的速度（每個樣本）
    static constexpr float kFollowBelow = 0.35f;  // 角度小於約 20 度才追

    void reset()
    {
        valid = false;
        angle = 0.0f;
    }

    // l／r：左右手的位置（公尺，y 朝上）。回傳 true＝*out 是展開後的角度；false＝目前算不出來（維持搖桿）
    bool step(const float l[3], const float r[3], bool bothTracked, float* out)
    {
        if (!bothTracked) {
            reset();
            return false;
        }
        const float vx = r[0] - l[0];
        const float vy = r[1] - l[1];
        const float vz = r[2] - l[2];
        const float h = std::sqrt(vx * vx + vz * vz);
        if (std::sqrt(h * h + vy * vy) < kMinSpanM) {
            if (valid) {
                *out = angle;
                return true;
            }
            return false;
        }
        if (axisX == 0.0f && axisZ == 0.0f) {
            if (h < kMinSpanM || std::fabs(vy) > h) {
                return false;  // 第一次要兩手大致水平才建立方向盤平面
            }
            axisX = vx / h;
            axisZ = vz / h;
        }
        const float along = vx * axisX + vz * axisZ;
        const float raw = -std::atan2(vy, along);  // 右手往下＝右轉＝正
        if (!valid) {
            angle = raw;
            valid = true;
        }
        else {
            float d = raw - std::remainder(angle, 2.0f * kPi);
            d = std::remainder(d, 2.0f * kPi);
            angle += d;
        }
        // 擺正時讓水平軸慢慢對齊兩手連線（轉身、換姿勢）；只追同方向，免得轉 180 度時翻面
        if (std::fabs(angle) < kFollowBelow && h >= kMinSpanM && along > 0.0f) {
            const float nx = axisX + (vx / h - axisX) * kFollow;
            const float nz = axisZ + (vz / h - axisZ) * kFollow;
            const float n = std::sqrt(nx * nx + nz * nz);
            if (n > 1e-4f) {
                axisX = nx / n;
                axisZ = nz / n;
            }
        }
        *out = angle;
        return true;
    }
};

// 方向盤角度 → 左搖桿 X。rangeDeg＝左打到底到右打到底的總角度
inline int16_t toStick(float angleRad, int rangeDeg)
{
    const float half = static_cast<float>(rangeDeg > 0 ? rangeDeg : 360) * 0.5f * kPi / 180.0f;
    float v = angleRad / half;
    v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

// 長按偵測：連續按住 holdNs 觸發一次，放開後才能再觸發
struct Hold {
    uint64_t sinceNs = 0;
    bool fired = false;

    // 回傳 true＝這一刻觸發
    bool step(bool pressed, uint64_t nowNs, uint64_t holdNs)
    {
        if (!pressed) {
            sinceNs = 0;
            fired = false;
            return false;
        }
        if (sinceNs == 0) {
            sinceNs = nowNs;
        }
        if (!fired && nowNs - sinceNs >= holdNs) {
            fired = true;
            return true;
        }
        return false;
    }
    bool held() const { return sinceNs != 0; }
};

}  // namespace xrwheel

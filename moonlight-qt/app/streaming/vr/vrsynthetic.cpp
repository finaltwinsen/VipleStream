#include "vrsynthetic.h"

#include <cmath>
#include <cstring>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr float kHeadHeightM = 1.6f;

// 先 yaw（繞 +Y）再 pitch（繞 +X）：q = qYaw * qPitch，存成 x, y, z, w
void yawPitchToQuat(double yaw, double pitch, float q[4])
{
    const double cy = std::cos(yaw * 0.5), sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5), sp = std::sin(pitch * 0.5);
    q[0] = (float)(cy * sp);
    q[1] = (float)(sy * cp);
    q[2] = (float)(-sy * sp);
    q[3] = (float)(cy * cp);
}

void setPose(VIPLE_VR_POSE* pose, const float pos[3], const float rot[4],
             const float linVel[3], const float angVel[3])
{
    memcpy(pose->pos, pos, sizeof(pose->pos));
    memcpy(pose->rot, rot, sizeof(pose->rot));
    memcpy(pose->linVel, linVel, sizeof(pose->linVel));
    memcpy(pose->angVel, angVel, sizeof(pose->angVel));
}

} // namespace

bool vrSyntheticMotionFromString(const QString& name, VrSyntheticMotion* out)
{
    const QString n = name.trimmed().toLower();
    if (n == "still") {
        *out = VrSyntheticMotion::Still;
    }
    else if (n == "sine") {
        *out = VrSyntheticMotion::Sine;
    }
    else if (n == "yaw30") {
        *out = VrSyntheticMotion::Yaw30;
    }
    else if (n == "tilt30yaw") {
        *out = VrSyntheticMotion::Tilt30Yaw;
    }
    else if (n == "fast") {
        *out = VrSyntheticMotion::Fast;
    }
    else {
        return false;
    }
    return true;
}

const char* vrSyntheticMotionName(VrSyntheticMotion motion)
{
    switch (motion) {
    case VrSyntheticMotion::Still: return "still";
    case VrSyntheticMotion::Sine:  return "sine";
    case VrSyntheticMotion::Yaw30: return "yaw30";
    case VrSyntheticMotion::Tilt30Yaw: return "tilt30yaw";
    case VrSyntheticMotion::Fast: return "fast";
    }
    return "unknown";
}

void vrSyntheticFill(VrSyntheticMotion motion, double t, VIPLE_VR_TRACKING* out)
{
    float headPos[3] = { 0.0f, kHeadHeightM, 0.0f };
    float headLinVel[3] = { 0.0f, 0.0f, 0.0f };
    float headAngVel[3] = { 0.0f, 0.0f, 0.0f };
    double yaw = 0.0, pitch = 0.0;

    switch (motion) {
    case VrSyntheticMotion::Still:
        break;
    case VrSyntheticMotion::Sine:
    case VrSyntheticMotion::Fast: {
        // Sine：位移 x 振幅 5 cm @0.5 Hz、y 振幅 2 cm @0.7 Hz；轉頭 yaw ±20° @0.25 Hz、pitch ±10° @0.4 Hz
        // Fast：位移 x 15 cm @1 Hz、y 5 cm @1.3 Hz；轉頭 yaw ±30° @1.2 Hz、pitch ±15° @1.7 Hz
        const bool fast = motion == VrSyntheticMotion::Fast;
        const double ax = fast ? 0.15 : 0.05, ay = fast ? 0.05 : 0.02;
        const double wx = 2 * kPi * (fast ? 1.0 : 0.5), wy = 2 * kPi * (fast ? 1.3 : 0.7);
        const double aYaw = (fast ? 30.0 : 20.0) * kDegToRad, aPitch = (fast ? 15.0 : 10.0) * kDegToRad;
        const double wyaw = 2 * kPi * (fast ? 1.2 : 0.25), wpitch = 2 * kPi * (fast ? 1.7 : 0.4);
        headPos[0] = (float)(ax * std::sin(wx * t));
        headPos[1] = (float)(kHeadHeightM + ay * std::sin(wy * t));
        headLinVel[0] = (float)(ax * wx * std::cos(wx * t));
        headLinVel[1] = (float)(ay * wy * std::cos(wy * t));
        yaw = aYaw * std::sin(wyaw * t);
        pitch = aPitch * std::sin(wpitch * t);
        {
            const double dYaw = aYaw * wyaw * std::cos(wyaw * t);
            const double dPitch = aPitch * wpitch * std::cos(wpitch * t);
            if (fast) {
                // 姿態是 q = qYaw·qPitch，世界座標的角速度＝dYaw·Y＋R_y(yaw)·(dPitch·X)。Fast 的擺頭幅度與轉速都大，
                // 照 Sine 的近似寫法會和姿態真正的導數差到 80°/s（幾乎全在 roll 軸），拿它量預測準不準會多出真人沒有的誤差
                headAngVel[0] = (float)(dPitch * std::cos(yaw));
                headAngVel[1] = (float)dYaw;
                headAngVel[2] = (float)(-dPitch * std::sin(yaw));
            }
            else {
                // Sine：拿歐拉角的變化率當世界座標角速度的近似（yaw 只有 ±20°，最多差 8.7°/s）。維持原樣，
                // 既有的 S0 基準才比得回去
                headAngVel[1] = (float)dYaw;
                headAngVel[0] = (float)dPitch;
            }
        }
        break;
    }
    case VrSyntheticMotion::Yaw30:
        yaw = std::fmod(30.0 * kDegToRad * t, 2 * kPi);
        headAngVel[1] = (float)(30.0 * kDegToRad);
        break;
    case VrSyntheticMotion::Tilt30Yaw: {
        // q = qYaw(t)·qPitch(30°)：先繞本地 X 傾 30°，再繞世界 Y 轉 → 世界座標的角速度就是 (0, ω, 0)（client 送的語意）
        yaw = std::fmod(30.0 * kDegToRad * t, 2 * kPi);
        pitch = 30.0 * kDegToRad;
        headAngVel[1] = (float)(30.0 * kDegToRad);
        const double wx = 2 * kPi * 0.1;
        headPos[0] = (float)(0.2 * std::sin(wx * t));
        headLinVel[0] = (float)(0.2 * wx * std::cos(wx * t));
        break;
    }
    }

    float headRot[4];
    yawPitchToQuat(yaw, pitch, headRot);

    out->flags = VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_LEFT | VIPLE_VR_TRK_RIGHT | VIPLE_VR_TRK_PRESENCE;
    setPose(&out->pose[VIPLE_VR_POSE_HMD], headPos, headRot, headLinVel, headAngVel);

    // 控制器：跟著頭部平移，固定在頭部前下方左右各 20 cm，不旋轉
    const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    const float leftPos[3] = { headPos[0] - 0.2f, headPos[1] - 0.3f, headPos[2] - 0.3f };
    const float rightPos[3] = { headPos[0] + 0.2f, headPos[1] - 0.3f, headPos[2] - 0.3f };
    setPose(&out->pose[VIPLE_VR_POSE_LEFT], leftPos, identity, headLinVel, zero);
    if (motion == VrSyntheticMotion::Tilt30Yaw) {
        // vr_probe --mode predict 比對 HMD 與右手的 poseTimeOffset：右手的姿態、速度與頭完全相同
        setPose(&out->pose[VIPLE_VR_POSE_RIGHT], rightPos, headRot, headLinVel, headAngVel);
    }
    else {
        setPose(&out->pose[VIPLE_VR_POSE_RIGHT], rightPos, identity, headLinVel, zero);
    }

    for (int i = 0; i < 2; i++) {
        memset(&out->input[i], 0, sizeof(out->input[i]));
        out->input[i].battery = 100;
        out->input[i].flags = VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
    }
    memset(&out->gaze, 0, sizeof(out->gaze));
}

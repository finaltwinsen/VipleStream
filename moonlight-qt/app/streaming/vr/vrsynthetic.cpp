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
    case VrSyntheticMotion::Sine: {
        // 位移：x 振幅 5 cm @0.5 Hz、y 振幅 2 cm @0.7 Hz
        const double wx = 2 * kPi * 0.5, wy = 2 * kPi * 0.7;
        headPos[0] = (float)(0.05 * std::sin(wx * t));
        headPos[1] = (float)(kHeadHeightM + 0.02 * std::sin(wy * t));
        headLinVel[0] = (float)(0.05 * wx * std::cos(wx * t));
        headLinVel[1] = (float)(0.02 * wy * std::cos(wy * t));
        // 轉頭：yaw ±20° @0.25 Hz、pitch ±10° @0.4 Hz
        const double wyaw = 2 * kPi * 0.25, wpitch = 2 * kPi * 0.4;
        yaw = 20.0 * kDegToRad * std::sin(wyaw * t);
        pitch = 10.0 * kDegToRad * std::sin(wpitch * t);
        // 角速度（世界座標的近似值：yaw 繞 Y、pitch 繞 X）
        headAngVel[1] = (float)(20.0 * kDegToRad * wyaw * std::cos(wyaw * t));
        headAngVel[0] = (float)(10.0 * kDegToRad * wpitch * std::cos(wpitch * t));
        break;
    }
    case VrSyntheticMotion::Yaw30:
        yaw = std::fmod(30.0 * kDegToRad * t, 2 * kPi);
        headAngVel[1] = (float)(30.0 * kDegToRad);
        break;
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
    setPose(&out->pose[VIPLE_VR_POSE_RIGHT], rightPos, identity, headLinVel, zero);

    for (int i = 0; i < 2; i++) {
        memset(&out->input[i], 0, sizeof(out->input[i]));
        out->input[i].battery = 100;
        out->input[i].flags = VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
    }
    memset(&out->gaze, 0, sizeof(out->gaze));
}

#include "qxr_synthetic.h"

#include <android/log.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegToRad = kPi / 180.0;
constexpr float kHeadHeightM = 1.6f;

// 先 yaw（繞 +Y）再 pitch（繞 +X）：q = qYaw * qPitch，存成 x, y, z, w。與 Qt yawPitchToQuat 同一組式子。
void yawPitchToQuat(double yaw, double pitch, float q[4])
{
    const double cy = std::cos(yaw * 0.5);
    const double sy = std::sin(yaw * 0.5);
    const double cp = std::cos(pitch * 0.5);
    const double sp = std::sin(pitch * 0.5);
    q[0] = static_cast<float>(cy * sp);
    q[1] = static_cast<float>(sy * cp);
    q[2] = static_cast<float>(-sy * sp);
    q[3] = static_cast<float>(cy * cp);
}

void setPose(VIPLE_VR_POSE* pose, const float pos[3], const float rot[4],
             const float linVel[3], const float angVel[3])
{
    std::memcpy(pose->pos, pos, sizeof(pose->pos));
    std::memcpy(pose->rot, rot, sizeof(pose->rot));
    std::memcpy(pose->linVel, linVel, sizeof(pose->linVel));
    std::memcpy(pose->angVel, angVel, sizeof(pose->angVel));
}

void fillHead(QxrSyntheticMotion motion, double t, float headPos[3], float headRot[4],
              float headLinVel[3], float headAngVel[3])
{
    headPos[0] = 0.0f;
    headPos[1] = kHeadHeightM;
    headPos[2] = 0.0f;
    for (int i = 0; i < 3; i++) {
        headLinVel[i] = 0.0f;
        headAngVel[i] = 0.0f;
    }
    double yaw = 0.0;
    double pitch = 0.0;

    switch (motion) {
    case QxrSyntheticMotion::Still:
        break;
    case QxrSyntheticMotion::Sine:
    case QxrSyntheticMotion::Fast: {
        // Sine：x ±5 cm @0.5 Hz、y ±2 cm @0.7 Hz、yaw ±20° @0.25 Hz、pitch ±10° @0.4 Hz。
        // Fast：x ±15 cm @1 Hz、y ±5 cm @1.3 Hz、yaw ±30° @1.2 Hz、pitch ±15° @1.7 Hz。
        const bool fast = motion == QxrSyntheticMotion::Fast;
        const double ax = fast ? 0.15 : 0.05;
        const double ay = fast ? 0.05 : 0.02;
        const double wx = 2 * kPi * (fast ? 1.0 : 0.5);
        const double wy = 2 * kPi * (fast ? 1.3 : 0.7);
        const double aYaw = (fast ? 30.0 : 20.0) * kDegToRad;
        const double aPitch = (fast ? 15.0 : 10.0) * kDegToRad;
        const double wyaw = 2 * kPi * (fast ? 1.2 : 0.25);
        const double wpitch = 2 * kPi * (fast ? 1.7 : 0.4);
        headPos[0] = static_cast<float>(ax * std::sin(wx * t));
        headPos[1] = static_cast<float>(kHeadHeightM + ay * std::sin(wy * t));
        headLinVel[0] = static_cast<float>(ax * wx * std::cos(wx * t));
        headLinVel[1] = static_cast<float>(ay * wy * std::cos(wy * t));
        yaw = aYaw * std::sin(wyaw * t);
        pitch = aPitch * std::sin(wpitch * t);
        {
            const double dYaw = aYaw * wyaw * std::cos(wyaw * t);
            const double dPitch = aPitch * wpitch * std::cos(wpitch * t);
            if (fast) {
                // q = qYaw·qPitch 的世界角速度。Fast 幅度大，不能用 Sine 那個歐拉近似。
                headAngVel[0] = static_cast<float>(dPitch * std::cos(yaw));
                headAngVel[1] = static_cast<float>(dYaw);
                headAngVel[2] = static_cast<float>(-dPitch * std::sin(yaw));
            }
            else {
                // Sine 維持歐拉變化率近似，既有的 host 基準才比得回去。
                headAngVel[1] = static_cast<float>(dYaw);
                headAngVel[0] = static_cast<float>(dPitch);
            }
        }
        break;
    }
    case QxrSyntheticMotion::Yaw30:
        yaw = std::fmod(30.0 * kDegToRad * t, 2 * kPi);
        headAngVel[1] = static_cast<float>(30.0 * kDegToRad);
        break;
    case QxrSyntheticMotion::Tilt30Yaw: {
        // 先繞本地 X 傾 30°，再以 30°/s 繞世界 Y。世界角速度就是 (0, ω, 0)。x 方向 ±20 cm @0.1 Hz。
        yaw = std::fmod(30.0 * kDegToRad * t, 2 * kPi);
        pitch = 30.0 * kDegToRad;
        headAngVel[1] = static_cast<float>(30.0 * kDegToRad);
        const double wx = 2 * kPi * 0.1;
        headPos[0] = static_cast<float>(0.2 * std::sin(wx * t));
        headLinVel[0] = static_cast<float>(0.2 * wx * std::cos(wx * t));
        break;
    }
    }

    yawPitchToQuat(yaw, pitch, headRot);
}

void fillTracking(QxrSyntheticMotion motion, double t, VIPLE_VR_TRACKING* out)
{
    float headPos[3];
    float headRot[4];
    float headLinVel[3];
    float headAngVel[3];
    fillHead(motion, t, headPos, headRot, headLinVel, headAngVel);

    out->flags = VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_LEFT | VIPLE_VR_TRK_RIGHT | VIPLE_VR_TRK_PRESENCE;
    setPose(&out->pose[VIPLE_VR_POSE_HMD], headPos, headRot, headLinVel, headAngVel);

    // 控制器跟著頭平移，固定在頭的前下方左右各 20 cm。除了 Tilt30Yaw 的右手，旋轉是單位四元數。
    const float identity[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float zero[3] = {0.0f, 0.0f, 0.0f};
    const float leftPos[3] = {headPos[0] - 0.2f, headPos[1] - 0.3f, headPos[2] - 0.3f};
    const float rightPos[3] = {headPos[0] + 0.2f, headPos[1] - 0.3f, headPos[2] - 0.3f};
    setPose(&out->pose[VIPLE_VR_POSE_LEFT], leftPos, identity, headLinVel, zero);
    if (motion == QxrSyntheticMotion::Tilt30Yaw) {
        setPose(&out->pose[VIPLE_VR_POSE_RIGHT], rightPos, headRot, headLinVel, headAngVel);
    }
    else {
        setPose(&out->pose[VIPLE_VR_POSE_RIGHT], rightPos, identity, headLinVel, zero);
    }

    for (int i = 0; i < 2; i++) {
        std::memset(&out->input[i], 0, sizeof(out->input[i]));
        out->input[i].battery = 100;
        out->input[i].flags = VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
        // §QUEST-VR：Quest 走 Touch。Qt 的合成填法把 profile 留 0，pad test 才寫死 Frame，這裡不沿用。
        out->input[i].profile = VIPLE_VR_CTRL_PROFILE_TOUCH;
    }
    std::memset(&out->gaze, 0, sizeof(out->gaze));
}

std::string trimLower(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

QxrSyntheticMotion motionFromName(std::string_view text)
{
    const std::string name = trimLower(text);
    if (name == "still") {
        return QxrSyntheticMotion::Still;
    }
    if (name == "sine") {
        return QxrSyntheticMotion::Sine;
    }
    if (name == "yaw30") {
        return QxrSyntheticMotion::Yaw30;
    }
    if (name == "tilt30yaw") {
        return QxrSyntheticMotion::Tilt30Yaw;
    }
    if (name == "fast") {
        return QxrSyntheticMotion::Fast;
    }
    __android_log_print(ANDROID_LOG_INFO, "viple-openxr",
                        "[VIPLE-VR-TRK] unknown motion '%s'; using sine",
                        name.empty() ? "" : name.c_str());
    return QxrSyntheticMotion::Sine;
}

}  // namespace

QxrSyntheticPose::QxrSyntheticPose(std::string_view motionName)
    : motion(motionFromName(motionName))
{
}

const char* QxrSyntheticPose::motionName() const
{
    switch (motion) {
    case QxrSyntheticMotion::Still:
        return "still";
    case QxrSyntheticMotion::Sine:
        return "sine";
    case QxrSyntheticMotion::Yaw30:
        return "yaw30";
    case QxrSyntheticMotion::Tilt30Yaw:
        return "tilt30yaw";
    case QxrSyntheticMotion::Fast:
        return "fast";
    }
    return "sine";
}

bool QxrSyntheticPose::sample(VIPLE_VR_TRACKING* out, uint64_t nowUs)
{
    if (out == nullptr) {
        return false;
    }
    if (!started) {
        started = true;
        startUs = nowUs;
    }
    // §QUEST-VR：t 對到 sampleTimeNs+predictNs。host 不會再補這一段預測（Qt §REVIEW-1104）。
    const double tSec = static_cast<double>(static_cast<int64_t>(nowUs - startUs)) * 1e-6
                        + static_cast<double>(out->predictNs) * 1e-9;
    fillTracking(motion, tSec, out);
    return true;
}

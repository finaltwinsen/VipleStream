#include "vrsynthetic.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

void vrSyntheticHead(VrSyntheticMotion motion, double t, float headPos[3], float headRot[4], float headLinVel[3], float headAngVel[3])
{
    headPos[0] = 0.0f;
    headPos[1] = kHeadHeightM;
    headPos[2] = 0.0f;
    for (int i = 0; i < 3; i++) {
        headLinVel[i] = 0.0f;
        headAngVel[i] = 0.0f;
    }
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

    yawPitchToQuat(yaw, pitch, headRot);
}

void VrSyntheticHeadTrack::poseAt(uint64_t clientNs, VIPLE_VR_POSE* out) const
{
    // 以整數相減再換成秒（起點之前是負的）
    const double t = static_cast<double>(static_cast<int64_t>(clientNs - startNs)) * 1e-9;
    float pos[3], rot[4], linVel[3], angVel[3];
    vrSyntheticHead(motion, t, pos, rot, linVel, angVel);
    setPose(out, pos, rot, linVel, angVel);
}

void vrSyntheticFill(VrSyntheticMotion motion, double t, VIPLE_VR_TRACKING* out)
{
    float headPos[3], headRot[4], headLinVel[3], headAngVel[3];
    vrSyntheticHead(motion, t, headPos, headRot, headLinVel, headAngVel);

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

void vrSyntheticHands(double t, VIPLE_VR_TRACKING* out)
{
    const VIPLE_VR_POSE& head = out->pose[VIPLE_VR_POSE_HMD];
    // 只取頭的 yaw（頭是 qYaw·qPitch）：手跟著身體的朝向，不跟著低頭抬頭
    const double qx = head.rot[0], qy = head.rot[1], qz = head.rot[2], qw = head.rot[3];
    const double yaw = std::atan2(2.0 * (qw * qy + qx * qz), 1.0 - 2.0 * (qx * qx + qy * qy));
    const double dYaw = head.angVel[1];
    const double cy = std::cos(yaw), sy = std::sin(yaw);

    for (int hand = 0; hand < 2; hand++) {
        const bool right = hand == 1;
        // 身體座標（x 右、y 上、z 後）裡的位置與速度
        double p[3], v[3], roll = 0.0, dRoll = 0.0;
        if (right) {
            const double w = 2 * kPi * 1.3, wv = 2 * kPi * 0.9;
            p[0] = 0.20 + 0.25 * std::sin(w * t);
            p[1] = -0.25 + 0.12 * std::sin(wv * t);
            p[2] = -0.45 - 0.10 * std::cos(w * t);
            v[0] = 0.25 * w * std::cos(w * t);
            v[1] = 0.12 * wv * std::cos(wv * t);
            v[2] = 0.10 * w * std::sin(w * t);
            roll = 40.0 * kDegToRad * std::sin(w * t);
            dRoll = 40.0 * kDegToRad * w * std::cos(w * t);
        }
        else {
            const double w = 2 * kPi * 0.7;
            p[0] = -0.22;
            p[1] = -0.30 + 0.08 * std::sin(w * t);
            p[2] = -0.40;
            v[0] = 0.0;
            v[1] = 0.08 * w * std::cos(w * t);
            v[2] = 0.0;
        }
        // 轉到世界座標：r = R_y(yaw)·p；速度＝頭的線速度＋R_y(yaw)·v＋(0,dYaw,0)×r
        const double rx = cy * p[0] + sy * p[2], rz = -sy * p[0] + cy * p[2];
        const double vx = cy * v[0] + sy * v[2], vz = -sy * v[0] + cy * v[2];
        const float pos[3] = { (float)(head.pos[0] + rx), (float)(head.pos[1] + p[1]), (float)(head.pos[2] + rz) };
        const float lin[3] = { (float)(head.linVel[0] + vx + dYaw * rz), (float)(head.linVel[1] + v[1]), (float)(head.linVel[2] + vz - dYaw * rx) };
        // 姿態 q = qYaw·qRollZ；世界座標的角速度＝dYaw·Y＋R_y(yaw)·(dRoll·Z)
        const double chy = std::cos(yaw / 2), shy = std::sin(yaw / 2), cr = std::cos(roll / 2), sr = std::sin(roll / 2);
        const float rot[4] = { (float)(shy * sr), (float)(shy * cr), (float)(chy * sr), (float)(chy * cr) };
        const float ang[3] = { (float)(dRoll * sy), (float)dYaw, (float)(dRoll * cy) };
        setPose(&out->pose[right ? VIPLE_VR_POSE_RIGHT : VIPLE_VR_POSE_LEFT], pos, rot, lin, ang);
        out->input[hand].flags |= VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
        if (out->input[hand].battery == 0) {
            out->input[hand].battery = 100;
        }
    }
    out->flags |= VIPLE_VR_TRK_LEFT | VIPLE_VR_TRK_RIGHT;
}

void vrSyntheticToss(double t, VIPLE_VR_TRACKING* out)
{
    const VIPLE_VR_POSE& head = out->pose[VIPLE_VR_POSE_HMD];
    // 人拋球的樣子：手加速往上、在最快的那一刻放開、放開之後 0.12 秒內就停住。速度用 minimum-jerk 的形狀升降
    // （s(τ)=10τ³−15τ⁴+6τ⁵，頭尾的加速度都是 0），所以放開那一刻的加速度是 0、之後才急減速（平均 −16.7 m/s²）：
    // 放開的訊號晚 30／60／90 ms 才被遊戲看到時，手速已經是 1.79／1.00／0.21 m/s（真值 2.00）
    constexpr double kCycle = 2.0, kHold = 0.5, kAccel = 0.30, kDecel = 0.12, kTop = 0.3, kFall = 0.6;
    constexpr double kPeak = 2.0, kReleaseAt = kHold + kAccel;
    constexpr double kUp = kPeak * kAccel * 0.5, kHeight = kUp + kPeak * kDecel * 0.5;
    const double u = t - std::floor(t / kCycle) * kCycle;
    // minimum-jerk：s(τ)=10τ³−15τ⁴+6τ⁵，s'(τ)=30τ²−60τ³+30τ⁴，∫s=2.5τ⁴−3τ⁵+τ⁶
    auto mj = [](double tau, double* ds) {
        tau = tau < 0.0 ? 0.0 : (tau > 1.0 ? 1.0 : tau);
        const double t2 = tau * tau, t3 = t2 * tau, t4 = t3 * tau, t5 = t4 * tau;
        *ds = 30.0 * t2 - 60.0 * t3 + 30.0 * t4;
        return 10.0 * t3 - 15.0 * t4 + 6.0 * t5;
    };
    auto mjInt = [](double tau) {
        tau = tau < 0.0 ? 0.0 : (tau > 1.0 ? 1.0 : tau);
        const double t4 = tau * tau * tau * tau;
        return 2.5 * t4 - 3.0 * t4 * tau + t4 * tau * tau;
    };
    double y = 0.0, vy = 0.0, ds = 0.0;
    if (u < kHold) {
        y = 0.0;
    }
    else if (u < kHold + kAccel) {
        // 速度 0 → kPeak
        const double tau = (u - kHold) / kAccel;
        vy = kPeak * mj(tau, &ds);
        y = kPeak * kAccel * mjInt(tau);
    }
    else if (u < kHold + kAccel + kDecel) {
        // 速度 kPeak → 0
        const double tau = (u - kHold - kAccel) / kDecel;
        vy = kPeak * (1.0 - mj(tau, &ds));
        y = kUp + kPeak * kDecel * (tau - mjInt(tau));
    }
    else if (u < kHold + kAccel + kDecel + kTop) {
        y = kHeight;
    }
    else if (u < kHold + kAccel + kDecel + kTop + kFall) {
        y = kHeight * (1.0 - mj((u - kHold - kAccel - kDecel - kTop) / kFall, &ds));
        vy = -kHeight * ds / kFall;
    }
    const float pos[3] = { head.pos[0] - 0.22f, (float)(head.pos[1] - 0.45 + y), head.pos[2] - 0.40f };
    const float lin[3] = { head.linVel[0], (float)(head.linVel[1] + vy), head.linVel[2] };
    const float identity[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    const float zero[3] = { 0.0f, 0.0f, 0.0f };
    setPose(&out->pose[VIPLE_VR_POSE_LEFT], pos, identity, lin, zero);
    VIPLE_VR_CONTROLLER_INPUT& left = out->input[0];
    left.flags |= VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
    if (left.battery == 0) {
        left.battery = 100;
    }
    if (u < kReleaseAt) {
        left.buttons |= VIPLE_VR_BTN_TRIGGER;
        left.trigger = 0xFFFF;
    }
    else {
        left.buttons &= ~static_cast<uint32_t>(VIPLE_VR_BTN_TRIGGER);
        left.trigger = 0;
    }
    out->flags |= VIPLE_VR_TRK_LEFT;
}

void vrSyntheticPadTest(double t, double startAt, VIPLE_VR_TRACKING* out)
{
    if (!(startAt > 0.0) || t < startAt) {
        return;
    }
    const double r = t - startAt;
    VIPLE_VR_CONTROLLER_INPUT& left = out->input[0];
    VIPLE_VR_CONTROLLER_INPUT& right = out->input[1];
    left.flags |= VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
    right.flags |= VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
    left.profile = VIPLE_VR_CTRL_PROFILE_FRAME;
    right.profile = VIPLE_VR_CTRL_PROFILE_FRAME;
    auto cycle = [&](double c) {
        if (c < 1.0) {
            right.buttons |= VIPLE_VR_BTN_A;
        }
        else if (c < 2.0) {
            right.buttons |= VIPLE_VR_BTN_TRIGGER;
            right.trigger = 0xFFFF;
            left.stickX = -32767;
        }
        else if (c < 3.0) {
            left.buttons |= VIPLE_VR_BTN_D;  // Frame 左手 dpad 上
            right.buttons |= VIPLE_VR_BTN_BUMPER;
        }
    };
    auto combo = [&]() {
        left.buttons |= VIPLE_VR_BTN_MENU;
        right.buttons |= VIPLE_VR_BTN_MENU;
    };
    if (r < 4.0) {
        cycle(r);
    }
    else if (r < 6.5) {
        combo();
    }
    else if (r >= 8.0 && r < 12.0) {
        cycle(r - 8.0);
    }
    else if (r >= 12.0 && r < 14.5) {
        combo();
    }
    else if (r >= 16.0 && r < 20.0) {
        cycle(r - 16.0);
    }
}

namespace {

// 第 k 段的目標（度）：固定的雜湊，每次執行都一樣
void synthGazeTarget(int64_t k, double* yawDeg, double* pitchDeg)
{
    uint64_t h = static_cast<uint64_t>(k) * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    const double u = static_cast<double>(h & 0xFFFFFu) / 1048575.0;
    const double v = static_cast<double>((h >> 20) & 0xFFFFFu) / 1048575.0;
    *yawDeg = (2.0 * u - 1.0) * 20.0;
    *pitchDeg = (2.0 * v - 1.0) * 12.0;
}

}  // namespace

void vrSyntheticGaze(double t, VIPLE_VR_TRACKING* out)
{
    if (t < 0.0) {
        t = 0.0;
    }
    // 眨眼：每 9 秒的第 4.00～4.15 秒沒有視線
    const double tb = std::fmod(t, 9.0);
    if (tb >= 4.0 && tb < 4.15) {
        memset(&out->gaze, 0, sizeof(out->gaze));
        out->flags &= ~VIPLE_VR_TRK_GAZE;
        return;
    }
    constexpr double kSlot = 0.7;
    const int64_t k = static_cast<int64_t>(t / kSlot);
    const double ts = t - static_cast<double>(k) * kSlot;
    double y0, p0, y1, p1;
    synthGazeTarget(k - 1, &y0, &p0);
    synthGazeTarget(k, &y1, &p1);
    double s;  // 0→1：從上一個目標到這一個目標走了多少
    const bool pursuit = (k % 8) == 5 || (k % 8) == 6;
    if (pursuit) {
        s = ts / kSlot;  // 追視：整段等速
    }
    else {
        // 掃視：時間＝20 ms＋每度 2.2 ms（20° 約 64 ms），位置照 minimum-jerk
        const double amp = std::sqrt((y1 - y0) * (y1 - y0) + (p1 - p0) * (p1 - p0));
        const double dur = 0.020 + 0.0022 * amp;
        const double x = ts >= dur ? 1.0 : ts / dur;
        s = x * x * x * (10.0 - 15.0 * x + 6.0 * x * x);
    }
    double yaw = y0 + (y1 - y0) * s;
    double pitch = p0 + (p1 - p0) * s;
    // 停住時的小抖動（追蹤器的雜訊）
    yaw += 0.15 * std::sin(2.0 * kPi * 7.3 * t);
    pitch += 0.15 * std::sin(2.0 * kPi * 11.1 * t + 1.0);
    out->gaze.yawF16 = VipleVrFloatToHalf(static_cast<float>(yaw * kDegToRad));
    out->gaze.pitchF16 = VipleVrFloatToHalf(static_cast<float>(pitch * kDegToRad));
    out->gaze.confidence = 255;
    out->gaze.flags = 0;
    out->gaze.sampleAgeMs = 0;
    out->flags |= VIPLE_VR_TRK_GAZE;
}

void vrSyntheticMenu(double t, double pressAt, VIPLE_VR_TRACKING* out)
{
    if (!(pressAt > 0.0) || t < pressAt) {
        return;
    }
    VIPLE_VR_CONTROLLER_INPUT& left = out->input[0];
    if (t < pressAt + 0.15) {
        left.buttons |= VIPLE_VR_BTN_MENU;
    }
    // MENU 是第 1 個鍵（pressCtr 的 bit 2..3）：按過之後計數比原值多 1（照協定填；host 目前只看 buttons 的準位，不讀 pressCtr）
    const uint32_t ctr = (((left.pressCtr >> 2) & 3u) + 1u) & 3u;
    left.pressCtr = (left.pressCtr & ~(3u << 2)) | (ctr << 2);
}

namespace {

struct PuppetPose {
    double pos[3];
    double ypr[3];  // 度
};

struct PuppetState {
    bool init = false;
    double lastReadT = -1.0, lastT = 0.0;
    double tau = 0.12;
    PuppetPose target[3], cur[3];  // 0 頭、1 左、2 右
    uint32_t buttons[2] = { 0, 0 };
    double trig[2] = { -1.0, -1.0 }, grip[2] = { -1.0, -1.0 };
    double stick[2][2] = { { 0, 0 }, { 0, 0 } };
};

PuppetState s_Puppet;

uint32_t puppetButtons(const std::string& v)
{
    static const struct { const char* name; uint32_t bit; } kNames[] = {
        { "trigger", VIPLE_VR_BTN_TRIGGER }, { "grip", VIPLE_VR_BTN_GRIP }, { "a", VIPLE_VR_BTN_A }, { "b", VIPLE_VR_BTN_B },
        { "menu", VIPLE_VR_BTN_MENU }, { "system", VIPLE_VR_BTN_SYSTEM }, { "stick", VIPLE_VR_BTN_THUMBSTICK },
        { "trackpad", VIPLE_VR_BTN_TRACKPAD }, { "c", VIPLE_VR_BTN_C }, { "d", VIPLE_VR_BTN_D }, { "bumper", VIPLE_VR_BTN_BUMPER },
    };
    uint32_t bits = 0;
    size_t i = 0;
    while (i <= v.size()) {
        size_t j = v.find(',', i);
        if (j == std::string::npos) {
            j = v.size();
        }
        std::string tok = v.substr(i, j - i);
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\r' || tok.back() == '\n' || tok.back() == '\t')) {
            tok.pop_back();
        }
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) {
            tok.erase(tok.begin());
        }
        for (const auto& n : kNames) {
            if (tok == n.name) {
                bits |= n.bit;
            }
        }
        i = j + 1;
    }
    return bits;
}

void puppetRead(const char* path, PuppetState* st)
{
    FILE* f = fopen(path, "rb");
    if (f == nullptr) {
        return;
    }
    char line[256];
    // 沒寫的按鍵類欄位＝放開；姿態沒寫＝維持
    uint32_t buttons[2] = { 0, 0 };
    double trig[2] = { -1.0, -1.0 }, grip[2] = { -1.0, -1.0 };
    double stick[2][2] = { { 0, 0 }, { 0, 0 } };
    bool any = false;
    while (fgets(line, sizeof(line), f) != nullptr) {
        char* eq = strchr(line, '=');
        if (eq == nullptr || line[0] == '#') {
            continue;
        }
        std::string key(line, eq - line);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        const char* val = eq + 1;
        double v[6] = { 0, 0, 0, 0, 0, 0 };
        const int n = sscanf(val, "%lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
        any = true;
        if (key == "head" || key == "left" || key == "right") {
            const int idx = key == "head" ? 0 : (key == "left" ? 1 : 2);
            if (n >= 3) {
                for (int k = 0; k < 3; k++) {
                    st->target[idx].pos[k] = v[k];
                    st->target[idx].ypr[k] = n >= 4 + k ? v[3 + k] : 0.0;
                }
            }
        }
        else if (key == "lbtn") {
            buttons[0] = puppetButtons(val);
        }
        else if (key == "rbtn") {
            buttons[1] = puppetButtons(val);
        }
        else if (key == "ltrig" && n >= 1) {
            trig[0] = v[0];
        }
        else if (key == "rtrig" && n >= 1) {
            trig[1] = v[0];
        }
        else if (key == "lgrip" && n >= 1) {
            grip[0] = v[0];
        }
        else if (key == "rgrip" && n >= 1) {
            grip[1] = v[0];
        }
        else if (key == "lstick" && n >= 2) {
            stick[0][0] = v[0];
            stick[0][1] = v[1];
        }
        else if (key == "rstick" && n >= 2) {
            stick[1][0] = v[0];
            stick[1][1] = v[1];
        }
        else if (key == "tau" && n >= 1 && v[0] >= 0.005 && v[0] <= 5.0) {
            st->tau = v[0];
        }
    }
    fclose(f);
    if (!any) {
        return;  // 寫到一半讀到空檔：這一次不算
    }
    for (int h = 0; h < 2; h++) {
        st->buttons[h] = buttons[h];
        st->trig[h] = trig[h];
        st->grip[h] = grip[h];
        st->stick[h][0] = stick[h][0];
        st->stick[h][1] = stick[h][1];
    }
}

// q = qYaw(Y)·qPitch(X)·qRoll(Z)，存成 x, y, z, w
void yprToQuat(const double yprDeg[3], float q[4])
{
    float yp[4];
    yawPitchToQuat(yprDeg[0] * kDegToRad, yprDeg[1] * kDegToRad, yp);
    const double r = yprDeg[2] * kDegToRad;
    const double rz = std::sin(r * 0.5), rw = std::cos(r * 0.5);
    // (yp) * (0, 0, rz, rw)
    q[0] = (float)(yp[0] * rw + yp[1] * rz);
    q[1] = (float)(yp[1] * rw - yp[0] * rz);
    q[2] = (float)(yp[2] * rw + yp[3] * rz);
    q[3] = (float)(yp[3] * rw - yp[2] * rz);
}

}  // namespace

void vrSyntheticPuppet(double t, const char* path, VIPLE_VR_TRACKING* out)
{
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    PuppetState& st = s_Puppet;
    if (!st.init) {
        st.init = true;
        const PuppetPose head = { { 0.0, kHeadHeightM, 0.0 }, { 0.0, 0.0, 0.0 } };
        const PuppetPose left = { { -0.20, kHeadHeightM - 0.35, -0.35 }, { 0.0, 0.0, 0.0 } };
        const PuppetPose right = { { 0.20, kHeadHeightM - 0.35, -0.35 }, { 0.0, 0.0, 0.0 } };
        st.target[0] = st.cur[0] = head;
        st.target[1] = st.cur[1] = left;
        st.target[2] = st.cur[2] = right;
        st.lastT = t;
    }
    if (st.lastReadT < 0.0 || t - st.lastReadT >= 0.05 || t < st.lastReadT) {
        st.lastReadT = t;
        puppetRead(path, &st);
    }
    double dt = t - st.lastT;
    st.lastT = t;
    if (dt < 0.0 || dt > 0.5) {
        dt = 0.0;
    }
    const double k = 1.0 - std::exp(-dt / st.tau);
    static const int kPoseIdx[3] = { VIPLE_VR_POSE_HMD, VIPLE_VR_POSE_LEFT, VIPLE_VR_POSE_RIGHT };
    for (int i = 0; i < 3; i++) {
        float pos[3], lin[3], rot[4];
        for (int a = 0; a < 3; a++) {
            const double err = st.target[i].pos[a] - st.cur[i].pos[a];
            st.cur[i].pos[a] += err * k;
            // 平滑之後還剩的誤差除以時間常數＝這一刻的速度
            lin[a] = (float)((st.target[i].pos[a] - st.cur[i].pos[a]) / st.tau);
            pos[a] = (float)st.cur[i].pos[a];
            st.cur[i].ypr[a] += (st.target[i].ypr[a] - st.cur[i].ypr[a]) * k;
        }
        // 真人的手不會完全不動：加一點很小的晃動（位置 ±1.5 mm、角度 ±0.3°，幾種不成整數比的頻率）。完全靜止的控制器
        // 有的遊戲會當成沒人拿著而把指標雷射收起來（Premium Bowling）。頭不加
        double ypr[3] = { st.cur[i].ypr[0], st.cur[i].ypr[1], st.cur[i].ypr[2] };
        if (i != 0) {
            const double ph = i * 1.7;
            const double w[3] = { 2 * kPi * 0.9, 2 * kPi * 1.3, 2 * kPi * 0.7 };
            for (int a = 0; a < 3; a++) {
                pos[a] += (float)(0.0015 * std::sin(w[a] * t + ph + a));
                lin[a] += (float)(0.0015 * w[a] * std::cos(w[a] * t + ph + a));
                ypr[a] += 0.3 * std::sin(w[(a + 1) % 3] * t + ph * 2 + a);
            }
        }
        yprToQuat(ypr, rot);
        const float zero[3] = { 0.0f, 0.0f, 0.0f };
        setPose(&out->pose[kPoseIdx[i]], pos, rot, lin, zero);
    }
    out->flags |= VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_LEFT | VIPLE_VR_TRK_RIGHT | VIPLE_VR_TRK_PRESENCE;
    for (int h = 0; h < 2; h++) {
        VIPLE_VR_CONTROLLER_INPUT& in = out->input[h];
        in.flags |= VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
        in.profile = VIPLE_VR_CTRL_PROFILE_FRAME;
        in.battery = 100;
        in.buttons = st.buttons[h];
        in.touches = st.buttons[h];
        auto axis = [](double v, bool pressed) {
            const double x = v >= 0.0 ? v : (pressed ? 1.0 : 0.0);
            return (uint16_t)(x <= 0.0 ? 0 : (x >= 1.0 ? 65535 : (int)(x * 65535.0)));
        };
        in.trigger = axis(st.trig[h], (st.buttons[h] & VIPLE_VR_BTN_TRIGGER) != 0);
        in.grip = axis(st.grip[h], (st.buttons[h] & VIPLE_VR_BTN_GRIP) != 0);
        auto stickv = [](double v) {
            const double x = v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v);
            return (int16_t)(x * 32767.0);
        };
        in.stickX = stickv(st.stick[h][0]);
        in.stickY = stickv(st.stick[h][1]);
    }
}

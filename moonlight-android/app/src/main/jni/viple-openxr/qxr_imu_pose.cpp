#include "qxr_imu_pose.h"

#include <android/log.h>

#include <cmath>
#include <cstring>

namespace {

constexpr float kHeadHeightM = 1.6f;
constexpr int64_t kExtrapolateLimitNs = 20000000;  // 20 ms
constexpr int64_t kStaleLimitNs = 200000000;       // 200 ms
constexpr uint64_t kLogIntervalNs = 1000000000ull;

void normalize(float q[4])
{
    const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (norm < 1.0e-8f) {
        q[0] = 0.0f;
        q[1] = 0.0f;
        q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    const float inv = 1.0f / norm;
    q[0] *= inv;
    q[1] *= inv;
    q[2] *= inv;
    q[3] *= inv;
}

// §VITURE：角速度在頭部座標。q_dot = 0.5 * q ⊗ ω，跟 Java 融合用同一組乘法，外插才不會反號。
void extrapolate(float q[4], const float w[3], double dt)
{
    const float qx = q[0];
    const float qy = q[1];
    const float qz = q[2];
    const float qw = q[3];
    const float wx = w[0];
    const float wy = w[1];
    const float wz = w[2];
    const float half = static_cast<float>(0.5 * dt);
    q[0] = qx + half * (qw * wx + qy * wz - qz * wy);
    q[1] = qy + half * (qw * wy - qx * wz + qz * wx);
    q[2] = qz + half * (qw * wz + qx * wy - qy * wx);
    q[3] = qw + half * (-qx * wx - qy * wy - qz * wz);
    normalize(q);
}

// q 是 x,y,z,w 的 body-to-world。ω_world = q · ω_body · q⁻¹。
// VIPLE_VR_POSE 的 pos 在 client 參考空間，angVel 跟它同一組座標（VipleVr.h）。
// 合成姿態填的是世界角速度（qxr_synthetic.cpp：Fast 的 qYaw·qPitch、Tilt30Yaw 的 (0, ω, 0)）。
// Qt sampleHmd 把 OpenXR 基準空間的 XrSpaceVelocity.angularVelocity 原樣送出。
void rotateBodyToWorld(const float q[4], const float v[3], float out[3])
{
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

void writePose(VIPLE_VR_POSE* pose, float x, float y, float z,
               float qx, float qy, float qz, float qw,
               float wx, float wy, float wz)
{
    pose->pos[0] = x;
    pose->pos[1] = y;
    pose->pos[2] = z;
    pose->rot[0] = qx;
    pose->rot[1] = qy;
    pose->rot[2] = qz;
    pose->rot[3] = qw;
    pose->linVel[0] = 0.0f;
    pose->linVel[1] = 0.0f;
    pose->linVel[2] = 0.0f;
    pose->angVel[0] = wx;
    pose->angVel[1] = wy;
    pose->angVel[2] = wz;
}

}  // namespace

void QxrImuPoseSource::push(float qw, float qx, float qy, float qz,
                            float wx, float wy, float wz, int64_t sourceTimeNs)
{
    // §VITURE：約 400 Hz。這裡只複製最新一筆，不能配置記憶體。
    std::lock_guard<std::mutex> guard(mu);
    q[0] = qx;
    q[1] = qy;
    q[2] = qz;
    q[3] = qw;
    w[0] = wx;
    w[1] = wy;
    w[2] = wz;
    sourceNs = sourceTimeNs;
    has = true;
}

void QxrImuPoseSource::logSkip(uint64_t nowUs)
{
    const uint64_t nowNs = nowUs * 1000ull;
    if (lastLogNs != 0 && nowNs - lastLogNs < kLogIntervalNs) {
        return;
    }
    lastLogNs = nowNs;
    __android_log_print(ANDROID_LOG_INFO, "VipleViture",
                        "[VIPLE-VITURE] imu sample skipped: missing or stale");
}

bool QxrImuPoseSource::sample(VIPLE_VR_TRACKING* out, uint64_t nowUs)
{
    if (out == nullptr) {
        return false;
    }
    float quat[4];
    float ang[3];
    int64_t sourceTime = 0;
    bool present = false;
    {
        std::lock_guard<std::mutex> guard(mu);
        present = has;
        if (present) {
            std::memcpy(quat, q, sizeof(quat));
            std::memcpy(ang, w, sizeof(ang));
            sourceTime = sourceNs;
        }
    }
    if (!present) {
        logSkip(nowUs);
        return false;
    }

    const int64_t nowNs = static_cast<int64_t>(nowUs * 1000ull);
    int64_t ageNs = nowNs - sourceTime;
    if (ageNs < 0) {
        ageNs = 0;
    }
    if (ageNs > kStaleLimitNs) {
        logSkip(nowUs);
        return false;
    }

    // §VITURE：超過 20 ms 就不要再外插，來源時間留著，上行端才不會把它改寫成現在。
    const bool extrapolateNow = ageNs <= kExtrapolateLimitNs;
    if (extrapolateNow && ageNs > 0) {
        extrapolate(quat, ang, static_cast<double>(ageNs) * 1.0e-9);
    }
    // 只外插到取樣當下（或來源時間）。predictNs 留著的話，host 會以為這筆已經含 3 個顯示週期。
    out->sampleTimeNs = extrapolateNow ? 0 : static_cast<uint64_t>(sourceTime);
    out->predictNs = 0;

    out->flags = VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_LEFT | VIPLE_VR_TRK_RIGHT | VIPLE_VR_TRK_PRESENCE;
    // 外插仍用機體角速度。送出前才轉到世界，軸才跟四元數同一組座標。
    float worldAng[3];
    rotateBodyToWorld(quat, ang, worldAng);
    // §VITURE：沒有位置追蹤。1.6 m 沿用合成姿態，當成坐著或站著的眼睛高度。
    writePose(&out->pose[VIPLE_VR_POSE_HMD], 0.0f, kHeadHeightM, 0.0f,
              quat[0], quat[1], quat[2], quat[3], worldAng[0], worldAng[1], worldAng[2]);

    // §VITURE：VipleVr.h 沒有「手把未連線」旗標。still 模式把兩手標成有效，放在頭的前下方。
    // 這裡沿用同一組位置，host 才不會把缺手把看成另一種姿態。
    const float identityW = 1.0f;
    writePose(&out->pose[VIPLE_VR_POSE_LEFT], -0.2f, kHeadHeightM - 0.3f, -0.3f,
              0.0f, 0.0f, 0.0f, identityW, 0.0f, 0.0f, 0.0f);
    writePose(&out->pose[VIPLE_VR_POSE_RIGHT], 0.2f, kHeadHeightM - 0.3f, -0.3f,
              0.0f, 0.0f, 0.0f, identityW, 0.0f, 0.0f, 0.0f);

    for (int i = 0; i < 2; i++) {
        std::memset(&out->input[i], 0, sizeof(out->input[i]));
        out->input[i].battery = 100;
        out->input[i].flags = VIPLE_VR_CTRL_ACTIVE | VIPLE_VR_CTRL_FOCUSED;
        out->input[i].profile = VIPLE_VR_CTRL_PROFILE_TOUCH;
    }
    std::memset(&out->gaze, 0, sizeof(out->gaze));
    return true;
}

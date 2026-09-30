// VipleStream 2.0 §VR M3a X3 — XrDesktopScreen 實作。設計見 xrdesktopscreen.h。

#include "xrdesktopscreen.h"

#include <cmath>

namespace {

constexpr float kPi = 3.14159265358979f;

// 以單位四元數旋轉向量
XrVector3f rotate(const XrQuaternionf& q, const XrVector3f& v)
{
    // t = 2·(q.xyz × v)；v' = v + w·t + q.xyz × t
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

XrQuaternionf yawQuat(float yaw)
{
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

}  // namespace

XrDesktopScreen::XrDesktopScreen(float distanceM, float fovDeg)
    : m_Distance(distanceM),
      m_FovRad(fovDeg * kPi / 180.0f),
      m_QuadWidth(2.0f * distanceM * std::tan(fovDeg * 0.5f * kPi / 180.0f))
{
}

XrDesktopScreen::Reason XrDesktopScreen::takePendingPlacement(bool focused)
{
    if (m_RecenterRequested.exchange(false, std::memory_order_acq_rel)) {
        return Reason::Recenter;
    }
    if (!m_Placed && focused) {
        return Reason::Initial;
    }
    return Reason::None;
}

void XrDesktopScreen::place(const XrPosef& head)
{
    // 頭部前方（-Z）投影到水平面；幾乎垂直看（投影長度太短）時保留上次的 yaw
    const XrVector3f fwd = rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    const float len = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (len > 1e-3f) {
        // yaw 旋轉把 (0,0,-1) 轉到 (-sin θ, 0, -cos θ)
        m_Yaw = std::atan2(-fwd.x / len, -fwd.z / len);
    }
    m_Head = head.position;
    m_Placed = true;
}

XrPosef XrDesktopScreen::quadPose() const
{
    XrPosef p;
    p.orientation = yawQuat(m_Yaw);
    p.position = {m_Head.x - std::sin(m_Yaw) * m_Distance,
                  m_Head.y,
                  m_Head.z - std::cos(m_Yaw) * m_Distance};
    return p;
}

XrPosef XrDesktopScreen::cylinderPose() const
{
    XrPosef p;
    p.orientation = yawQuat(m_Yaw);
    p.position = m_Head;
    return p;
}

XrPosef XrDesktopScreen::statusPose(float screenHeightM, float statusHeightM) const
{
    XrPosef p = quadPose();
    p.position.y += screenHeightM * 0.5f + statusHeightM * 0.5f + 0.03f;
    return p;
}

// VipleStream 2.0 §VR M3a X3 — XrDesktopScreen：β 虛擬螢幕的擺放（純數學＋狀態，不碰 OpenXR 物件）。
// 設計：vr_architecture.md §2.5（β：world-locked，前方 1.5 m、寬 60°；runtime 有
// XR_KHR_composition_layer_cylinder 就改 cylinder）。
//
// 擺放只取頭部的「水平朝向」（yaw）：螢幕永遠直立、與視線同高，頭歪或低頭 recenter 也不會斜掉。
// 位置與朝向都在 LOCAL 空間。第一次 FOCUSED 自動擺一次；recenter() 由任何執行緒呼叫，
// frame thread 下一幀依當下頭部 pose 重擺。

#pragma once

#include <openxr/openxr.h>

#include <atomic>

class XrDesktopScreen
{
public:
    enum class Reason { None, Initial, Recenter };

    XrDesktopScreen(float distanceM, float fovDeg);

    // 任何執行緒：要求下一幀重擺
    void requestRecenter() { m_RecenterRequested.store(true, std::memory_order_release); }

    // frame thread：是否需要（重新）擺放；需要時回傳原因並清掉要求旗標
    Reason takePendingPlacement(bool focused);

    // frame thread：依頭部 pose（LOCAL 空間）擺放
    void place(const XrPosef& head);

    bool placed() const { return m_Placed; }
    float yawRad() const { return m_Yaw; }

    // 螢幕寬（公尺）：quad 用弦長 2·d·tan(fov/2)；cylinder 用弧長 d·fov
    float quadWidthM() const { return m_QuadWidth; }
    float cylinderRadiusM() const { return m_Distance; }
    float cylinderAngleRad() const { return m_FovRad; }

    // quad：中心在頭部前方 d 公尺、與視線同高，法線朝向使用者
    XrPosef quadPose() const;
    // cylinder：中心在頭部位置（可見弧段以 pose 的 -Z 為中心）
    XrPosef cylinderPose() const;
    // 狀態 quad：螢幕上緣之上（螢幕高度 heightM），同一朝向
    XrPosef statusPose(float screenHeightM, float statusHeightM) const;

private:
    float m_Distance;
    float m_FovRad;
    float m_QuadWidth;
    bool m_Placed = false;
    std::atomic<bool> m_RecenterRequested{false};
    float m_Yaw = 0.0f;
    XrVector3f m_Head = {0.0f, 0.0f, 0.0f};
};

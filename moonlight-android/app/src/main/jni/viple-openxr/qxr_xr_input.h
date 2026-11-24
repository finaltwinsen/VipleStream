#pragma once

// §QUEST-PCVR：Quest 的 OpenXR 手把。按鍵的線上格式對齊 Frame 的 XrVrControllers，不帶 Qt／SDL。

#include "qxr_pose_source.h"

#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

struct QxrXrControllers {
    QxrXrControllers();
    ~QxrXrControllers();

    QxrXrControllers(const QxrXrControllers&) = delete;
    QxrXrControllers& operator=(const QxrXrControllers&) = delete;

    // touchPlus：instance 有啟用 XR_META_touch_controller_plus 才另送那份 profile。
    bool create(XrInstance instance, XrSession session, bool touchPlus, std::string* error);
    void destroy();

    XrActionSet actionSet() const { return set_; }

    // frame thread，xrSyncActions 之後。focused 為 false 時按鍵與類比值放開，pressCtr 不動。
    void updateButtons(bool focused);
    // frame thread，xrWaitFrame 之後。給上行 locate 失敗時沿用。
    void updateGripPose(XrTime predictedDisplayTime, XrSpace base);
    // 上行執行緒。t 為 0 時不 locate，沿用 updateGripPose 的姿態。
    void fill(VIPLE_VR_TRACKING* sample, XrTime t, XrSpace base);

    // 任意執行緒。hand 0＝左、1＝右。
    // amplitude <= 0 表示停止。durationNs 可以是 XR_MIN_HAPTIC_DURATION（-1），那是最短脈衝，不是停止。
    void queueHaptic(int hand, int64_t durationNs, float frequencyHz, float amplitude);
    // frame thread。
    void applyPendingHaptics();

    void onProfileChanged();

private:
    struct ComboState {
        uint64_t startNs = 0;
        bool active = false;
        uint32_t afterMask = 0;
    };

    static bool applySystemCombo(ComboState& st, uint64_t nowNs, uint32_t& buttons, uint32_t& touches, float& trigger);
    bool stateOk(XrResult result, const char* fn) const;

    struct Hand {
        uint32_t buttons = 0;
        uint32_t touches = 0;
        uint32_t pressCtr = 0;
        uint16_t trigger = 0;
        uint16_t grip = 0;
        int16_t stickX = 0;
        int16_t stickY = 0;
        bool poseActive = false;
        bool poseValid = false;
        float pos[3] = {};
        float rot[4] = {0, 0, 0, 1};
        float linVel[3] = {};
        float angVel[3] = {};
        bool linValid = false;
        bool angValid = false;
        ComboState combo;
        uint32_t edges = 0;
    };

    struct HapticReq {
        int hand = 0;
        int64_t durationNs = 0;
        float frequencyHz = 0;
        float amplitude = 0;
    };

    bool boolState(XrAction action, int hand) const;
    float floatState(XrAction action, int hand) const;
    XrVector2f vecState(XrAction action, int hand) const;
    bool poseIsActive(int hand) const;
    bool locateGrip(int hand, XrSpace base, XrTime t, Hand* out);
    void logStatsIfDue();

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSession session_ = XR_NULL_HANDLE;
    XrPath hands_[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrActionSet set_ = XR_NULL_HANDLE;
    XrAction gripPose_ = XR_NULL_HANDLE;
    XrAction aimPose_ = XR_NULL_HANDLE;
    XrAction triggerValue_ = XR_NULL_HANDLE;
    XrAction triggerClick_ = XR_NULL_HANDLE;
    XrAction triggerTouch_ = XR_NULL_HANDLE;
    XrAction squeezeValue_ = XR_NULL_HANDLE;
    XrAction squeezeClick_ = XR_NULL_HANDLE;
    XrAction stick_ = XR_NULL_HANDLE;
    XrAction stickClick_ = XR_NULL_HANDLE;
    XrAction stickTouch_ = XR_NULL_HANDLE;
    XrAction btnLo_ = XR_NULL_HANDLE;
    XrAction btnLoTouch_ = XR_NULL_HANDLE;
    XrAction btnHi_ = XR_NULL_HANDLE;
    XrAction btnHiTouch_ = XR_NULL_HANDLE;
    XrAction menu_ = XR_NULL_HANDLE;
    XrAction system_ = XR_NULL_HANDLE;
    XrAction thumbrestTouch_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE;
    XrSpace gripSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    std::mutex mu_;
    Hand hand_[2];
    bool focused_ = false;
    uint8_t profileWire_[2] = {VIPLE_VR_CTRL_PROFILE_UNKNOWN, VIPLE_VR_CTRL_PROFILE_UNKNOWN};
    char profilePath_[2][XR_MAX_PATH_LENGTH] = {"(none)", "(none)"};
    uint32_t statPoseN_[2] = {};
    uint32_t statPoseOk_[2] = {};
    uint32_t statSnapshots_ = 0;
    uint64_t statStartNs_ = 0;
    bool statClockStarted_ = false;
    mutable std::atomic<uint32_t> locateFails_{0};
    mutable std::atomic<uint32_t> stateFails_{0};

    std::mutex hapticMu_;
    std::deque<HapticReq> haptics_;
    uint32_t statHapticApplied_ = 0;
    uint32_t statHapticFailed_ = 0;
    bool hapticDropLogged_ = false;
};

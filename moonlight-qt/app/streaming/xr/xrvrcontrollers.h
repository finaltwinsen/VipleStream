#pragma once

// VipleStream 2.0 §VR M4a R2 — PCVR 控制器輸入（0x5506 L/R pose＋input）與 haptic。
//
// PCVR 模式時取代 β 的 XrInput（一個 session 只能 attach 一組 action set）：
// - grip pose（左右手，含速度）→ 0x5506 pose[LEFT/RIGHT]（server controller_device 自己套
//   raw_from_grip，client 不轉換）。
// - 按鍵 → buttons／touches（bit 依 docs/vr_protocol.md「控制器按鍵 bit」：b0 SYSTEM、b1 MENU、
//   b2 A/X、b3 B/Y、b4 THUMBSTICK、b5 TRIGGER、b6 GRIP、b7 TRACKPAD、b8 THUMBREST），pressCtr 每鍵
//   2 bit 的按下邊緣計數（鍵 i 在 bit 2i..2i+1，補償 unsequenced 丟包漏掉的短按），trigger／grip
//   0–65535、搖桿 ±32767、battery 255＝未知、flags b0 active（pose 有效）／b1 focused。
// - 沒有 FOCUSED：按鍵、類比值全部放開（server 看到 flags.focused=0 也會視為無輸入），pressCtr 不動。
// - 同一手 menu＋trigger 按住 1 s → SYSTEM（期間遮掉 MENU 與 TRIGGER），任一放開即結束。
// - haptic：main thread 經 queueHaptic 放進佇列，XR frame thread 呼叫 xrApplyHapticFeedback。
//
// 執行緒：create／destroy／updateFrame／applyPendingHaptics 在 XR frame thread（或 bring-up 執行緒，
// 早於 frame thread 啟動）；fill 在 tracking 送出執行緒；queueHaptic 在 main thread。狀態快照以
// m_Mutex 保護。

#ifdef HAVE_OPENXR

#include <openxr/openxr.h>

#include <QString>
#include <QStringList>

#include <cstdint>
#include <deque>
#include <mutex>

struct _VIPLE_VR_TRACKING;

class XrVrControllers
{
public:
    // testInput（dev）：不讀控制器按鍵，改用固定的合成按鍵序列（走同一條 0x5506 打包）
    explicit XrVrControllers(bool testInput);
    ~XrVrControllers();

    XrVrControllers(const XrVrControllers&) = delete;
    XrVrControllers& operator=(const XrVrControllers&) = delete;

    bool create(XrInstance instance, XrSession session, const QStringList& enabledExts, QString* error);
    void destroy();  // 要在 xrDestroySession 之前

    XrActionSet actionSet() const { return m_Set; }

    // frame thread，xrSyncActions 之後：讀按鍵狀態；trackSpace 非空時以 predictedDisplayTime locate
    // grip（frameloop 模式的 pose 樣本）
    void updateFrame(XrTime predictedDisplayTime, bool focused, XrSpace trackSpace);
    // tracking 送出執行緒：填 pose[LEFT/RIGHT]、input[0/1] 與 flags 的 LEFT/RIGHT bit。
    // thread 模式（xrTime != 0）當下 locate grip；否則沿用 updateFrame 最近一次的 pose。
    void fill(_VIPLE_VR_TRACKING* sample, XrSpace trackSpace, XrTime xrTime);

    // main thread：device 1＝左、2＝右（VIPLE_VR_POSE_LEFT/RIGHT）
    void queueHaptic(uint8_t device, uint32_t durationUs, float frequencyHz, float amplitude);
    // frame thread
    void applyPendingHaptics();

    void onProfileChanged();
    QString takeStatsLine();

private:
    struct Hand {
        // 按鍵快照（updateFrame 寫、fill 讀）
        uint32_t buttons = 0;
        uint32_t touches = 0;
        uint32_t pressCtr = 0;
        uint16_t trigger = 0;
        uint16_t grip = 0;
        int16_t stickX = 0;
        int16_t stickY = 0;
        bool poseActive = false;
        // frameloop pose
        bool poseValid = false;
        float pos[3] = {};
        float rot[4] = {0, 0, 0, 1};
        float linVel[3] = {};
        float angVel[3] = {};
        // system 組合
        uint64_t comboStartNs = 0;
        bool comboActive = false;
        // 統計
        uint32_t edges = 0;
    };

    bool boolState(XrAction a, int h) const;
    float floatState(XrAction a, int h) const;
    XrVector2f vecState(XrAction a, int h) const;
    bool poseIsActive(int h) const;
    bool locateGrip(int h, XrSpace trackSpace, XrTime t, Hand* out) const;
    void applyTestInput(int h, uint32_t* buttons, uint32_t* touches, float* trigger, float* grip,
                        float* sx, float* sy, uint64_t nowNs);

    bool m_TestInput;
    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSession m_Session = XR_NULL_HANDLE;
    XrPath m_Hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrActionSet m_Set = XR_NULL_HANDLE;
    XrAction m_GripPose = XR_NULL_HANDLE;
    XrAction m_TriggerValue = XR_NULL_HANDLE;
    XrAction m_TriggerClick = XR_NULL_HANDLE;
    XrAction m_TriggerTouch = XR_NULL_HANDLE;
    XrAction m_SqueezeValue = XR_NULL_HANDLE;
    XrAction m_SqueezeClick = XR_NULL_HANDLE;
    XrAction m_Stick = XR_NULL_HANDLE;
    XrAction m_StickClick = XR_NULL_HANDLE;
    XrAction m_StickTouch = XR_NULL_HANDLE;
    XrAction m_BtnLo = XR_NULL_HANDLE;       // A／X（左手 Frame：dpad_down）
    XrAction m_BtnLoTouch = XR_NULL_HANDLE;
    XrAction m_BtnHi = XR_NULL_HANDLE;       // B／Y（左手 Frame：dpad_up）
    XrAction m_BtnHiTouch = XR_NULL_HANDLE;
    XrAction m_Menu = XR_NULL_HANDLE;
    XrAction m_System = XR_NULL_HANDLE;
    XrAction m_ThumbrestTouch = XR_NULL_HANDLE;
    XrAction m_Haptic = XR_NULL_HANDLE;
    XrSpace m_GripSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    mutable std::mutex m_Mutex;
    Hand m_Hand[2];
    bool m_Focused = false;
    uint64_t m_CreatedNs = 0;

    struct HapticReq {
        uint8_t device;
        uint32_t durationUs;
        float frequencyHz;
        float amplitude;
    };
    std::mutex m_HapticMutex;
    std::deque<HapticReq> m_Haptics;

    // 10 s 統計
    uint32_t m_StatCombos = 0;
    uint32_t m_StatHapticApplied = 0;
    uint32_t m_StatHapticFailed = 0;
    uint32_t m_StatHapticDropped = 0;
    uint32_t m_StatFocusLoss = 0;
};

#endif // HAVE_OPENXR

#pragma once

#include "xrcombo.h"

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
// - §VR-KBD（2026-11-13）：VR 串流中的虛擬鍵盤。同一手 menu＋grip 按住 1 s 開關（和 menu＋trigger＝SYSTEM 同一種
//   操作）。鍵盤開著時按鍵與類比值一律不送給 host（姿態照送，遊戲裡的手還在），扳機改成按鍵盤上的鍵；開關的那一下
//   還按著的鍵遮到全部放開為止，不會變成遊戲裡的一次按下。版面、黏滯修飾鍵、中/英鍵的規則同 XR 桌面（xrkeyboard.h）。
//   鍵盤擺在哪、貼圖與高亮由 XrContext 負責，這裡只做開關、射線命中與按鍵事件。
//
// 執行緒：create／destroy／updateFrame／applyPendingHaptics 在 XR frame thread（或 bring-up 執行緒，
// 早於 frame thread 啟動）；fill 在 tracking 送出執行緒；queueHaptic 在 main thread。狀態快照以
// m_Mutex 保護。

#ifdef HAVE_OPENXR

#include "xrinput.h"  // §VR-KBD：XrRay（射線與 quad 求交）、XrKeyboard（版面）

#include <openxr/openxr.h>

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

struct _VIPLE_VR_TRACKING;

class XrVrControllers
{
public:
    // testInput（dev）：不讀控制器按鍵，改用固定的合成按鍵序列（走同一條 0x5506 打包）
    // exitCombo：§VR-EXIT-COMBO，A＋X＋扳機＋握把整組按住的上升緣（frame thread 呼叫）；空＝不偵測
    explicit XrVrControllers(bool testInput, std::function<void()> exitCombo = {});
    ~XrVrControllers();

    XrVrControllers(const XrVrControllers&) = delete;
    XrVrControllers& operator=(const XrVrControllers&) = delete;

    bool create(XrInstance instance, XrSession session, const QStringList& enabledExts, QString* error);
    void destroy();  // 要在 xrDestroySession 之前

    XrActionSet actionSet() const { return m_Set; }
    XrSpace eyeGazeSpace() const { return m_EyeGazeSpace; }

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

    // §VR-WHEEL：方向盤模式。wire＝input[0].reserved 的值（VIPLE_VR_WHEEL_ON | 轉向範圍/10，0＝關）；
    // toggled：兩支搖桿同時按住 1.5 秒切換時呼叫（frame thread），空＝不偵測快捷鍵
    void setWheel(uint8_t wire, std::function<void(bool on)> toggled);

    // ── §VR-KBD：虛擬鍵盤 ──
    // key：按鍵事件（Win32 VK＋Limelight MODIFIER_*，frame thread 呼叫）；空＝沒有鍵盤。
    // testText（dev --xr-test-keyboard）：開始 20 s 後自動打開鍵盤、逐字按完再關，不必有控制器
    void setKeyboard(std::function<void(int vk, bool down, uint8_t mods)> key, const QString& testText);
    // 任何執行緒：要求切換開關（frame thread 下一格處理）
    void requestKeyboardToggle() { m_KbToggleReq.fetch_add(1, std::memory_order_acq_rel); }
    // 以下都在 frame thread
    bool keyboardOpen() const { return m_KbOpen; }
    const XrKeyboard& keyboard() const { return m_Kb; }
    uint8_t stickyModifiers() const { return m_KbSticky; }
    int keyboardHover(bool* pressed) const
    {
        *pressed = m_KbActiveKey >= 0 && m_KbActiveKey == m_KbHover;
        return m_KbHover;
    }
    bool keyboardPointer(XrPosef* pose) const;
    // updateFrame 之後、鍵盤那一層擺好之後呼叫：shown＝這一格鍵盤有畫出來；pose／w／h＝鍵盤 quad（space 座標）
    // bar／barW／barH＝鍵盤下方那條白條的命中範圍（§VR-KBD-DRAG；比畫出來的大）
    void updateKeyboard(XrTime t, XrSpace space, bool shown, const XrPosef& pose, float w, float h, const XrPosef& bar, float barW,
                        float barH);
    // §VR-KBD-DRAG：0＝沒指著白條、1＝指著、2＝拖動中
    int keyboardBarState() const { return m_KbDragHand >= 0 ? 2 : (m_KbBarHover ? 1 : 0); }
    // 拖動中每一格回報鍵盤中心的新位置（取走就清掉）；朝向由呼叫端決定
    bool takeKeyboardMove(XrVector3f* center)
    {
        if (!m_KbMoveReq) {
            return false;
        }
        m_KbMoveReq = false;
        *center = m_KbMovePos;
        return true;
    }

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
        bool linValid = false;  // 2026-10-05：runtime 回報的速度旗標（無效時 linVel／angVel 送 0）
        bool angValid = false;
        // system 組合（規則在 xrcombo.h）
        xrcombo::State combo;
        // 統計
        uint32_t edges = 0;
    };

    void setKeyboardOpen(bool open, const char* why);
    void kbPress(int idx);
    void kbRelease();
    void kbSend(int vk, bool down, uint8_t mods);
    bool boolState(XrAction a, int h) const;
    float floatState(XrAction a, int h) const;
    XrVector2f vecState(XrAction a, int h) const;
    bool poseIsActive(int h) const;
    bool locateGrip(int h, XrSpace trackSpace, XrTime t, Hand* out) const;
    void applyTestInput(int h, uint32_t* buttons, uint32_t* touches, float* trigger, float* grip,
                        float* sx, float* sy, uint64_t nowNs);

    bool m_TestInput;
    std::function<void()> m_ExitCombo;  // §VR-EXIT-COMBO
    bool m_ExitComboWas = false;        // 上一格是否整組按住（frame thread）
    std::atomic<uint8_t> m_Wheel{0};     // §VR-WHEEL：input[0].reserved（frame thread 寫、tracking thread 讀）
    std::function<void(bool)> m_WheelToggled;
    uint64_t m_WheelComboStartNs = 0;   // frame thread
    bool m_WheelComboFired = false;
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
    XrAction m_BtnLo = XR_NULL_HANDLE;       // A／X（Frame：右 a、左 dpad_down）
    XrAction m_BtnLoTouch = XR_NULL_HANDLE;
    XrAction m_BtnHi = XR_NULL_HANDLE;       // B／Y（Frame：右 b、左 dpad_right）
    XrAction m_BtnHiTouch = XR_NULL_HANDLE;
    XrAction m_BtnC = XR_NULL_HANDLE;        // §VR-FRAME-CTRL：Frame 右 x、左 dpad_left
    XrAction m_BtnD = XR_NULL_HANDLE;        // §VR-FRAME-CTRL：Frame 右 y、左 dpad_up
    XrAction m_Bumper = XR_NULL_HANDLE;      // §VR-FRAME-CTRL：Frame shoulder
    XrAction m_Menu = XR_NULL_HANDLE;
    XrAction m_System = XR_NULL_HANDLE;
    XrAction m_ThumbrestTouch = XR_NULL_HANDLE;
    XrAction m_Haptic = XR_NULL_HANDLE;
    // §VR-EXIT-COMBO：X（Touch 在左手、Frame 在右手）。只給結束組合鍵用、不轉送；Frame 右手的 x 本來沒有綁
    XrAction m_ComboX = XR_NULL_HANDLE;
    XrSpace m_GripSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    // §VR-KBD（都只在 frame thread 用，m_KbToggleReq 除外）
    XrAction m_AimPose = XR_NULL_HANDLE;
    XrSpace m_AimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::function<void(int, bool, uint8_t)> m_KbKey;
    std::atomic<int> m_KbToggleReq{0};
    XrKeyboard m_Kb;
    bool m_KbOpen = false;
    bool m_KbMaskUntilRelease = false;  // 開關那一下還按著的鍵：全部放開之前不送給 host
    uint64_t m_KbComboStartNs[2] = {0, 0};
    bool m_KbComboFired[2] = {false, false};
    bool m_KbRawTrigger[2] = {false, false};  // 這一格真實的扳機（沒被遮掉的）
    bool m_KbTrigPrev[2] = {false, false};
    int m_KbHand = 1;            // 目前用哪一手指（最近按過扳機、或唯一指著鍵盤的那一手）
    int m_KbPressHand = -1;      // 按著鍵的那一手
    uint8_t m_KbSticky = 0;
    int m_KbHover = -1;
    int m_KbActiveKey = -1;
    uint8_t m_KbActiveMods = 0;
    bool m_KbHit = false;
    XrPosef m_KbHitPose = {};
    uint32_t m_StatKbKeys = 0;
    // §VR-KBD-DRAG
    int m_KbDragHand = -1;         // 抓著白條的那一手
    XrVector3f m_KbDragRel = {};   // 抓住那一刻鍵盤中心在那一手 aim 座標裡的位置
    bool m_KbBarHover = false;
    bool m_KbMoveReq = false;
    XrVector3f m_KbMovePos = {};
    uint32_t m_StatKbDrags = 0;
    bool m_KbTestMoved = false;
    // dev --xr-test-keyboard
    QString m_KbTestText;
    std::vector<int> m_KbTestKeys;
    int m_KbTestState = 0;  // 0＝還沒開、1＝按鍵中、2＝做完
    uint64_t m_KbTestShownNs = 0;
    XrAction m_EyeGazeAction = XR_NULL_HANDLE;
    XrSpace m_EyeGazeSpace = XR_NULL_HANDLE;

    mutable std::mutex m_Mutex;
    Hand m_Hand[2];
    // M4a 收尾：目前的 interaction profile（VIPLE_VR_CTRL_PROFILE_*，frame thread 寫、tracking thread 讀）
    std::atomic<uint8_t> m_Profile[2] = {{0}, {0}};
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
    std::atomic<uint32_t> m_StatHapticDropped{0};  // main thread counts, frame thread reads and clears
    uint32_t m_StatFocusLoss = 0;
    // 2026-10-05：送出樣本的控制器速度有效率與最大值（m_Mutex 保護；fill 在 tracking 送出執行緒）
    struct VelStat {
        uint32_t samples = 0;
        uint32_t linValid = 0;
        uint32_t angValid = 0;
        float linMax = 0.0f;  // m/s
        float angMax = 0.0f;  // rad/s
    } m_VelStat[2];
};

#endif // HAVE_OPENXR

// VipleStream 2.0 §VR M3a X4 — XrInput：β 的控制器射線滑鼠。
// 設計：vr_architecture.md β 輸入列（射線滑鼠 One-Euro、trigger 左鍵、grip 或 B 右鍵、搖桿 Y 捲動、
// XR FOCUSED 期間忽略平面滑鼠事件、失去 FOCUSED 送全部放開）。
//
// 分工：
//   - XrRay（純數學，可單元測試）：射線與 quad／cylinder 螢幕求交 → UV（0..1，左上為原點）、One-Euro 濾波。
//   - XrInput（只在 XR frame thread 上用）：action set 與綁定、aim pose 定位、慣用手（最近按過 trigger
//     的那隻）、求交、濾波、產生事件。事件經 Options 的 sink（Session 提供，推 SDL_USEREVENT 到 main
//     thread）交給 SdlInputHandler::handleXr*，最後送 LiSendMousePositionEvent／ButtonEvent／HighResScroll。
//   - dev：--xr-test-pointer 不讀控制器，合成一條從頭部射向螢幕、每 4 s 畫一個圓的射線，圓頂點合成一次
//     trigger 按下／放開；求交之後的路徑與真實輸入完全相同。
//
// 綁定：khr/simple_controller、oculus/touch_controller、valve/index_controller；runtime 有
// XR_VALVE_frame_controller_interaction 時另外建議 /interaction_profiles/valve/frame_controller_valve
// （路徑依 Valve OpenXR Unity 套件文件的 Steam Frame Controller Profile；openxr-loader 1.1.59 的標頭沒收錄
// 此擴充。面鍵左右不同：右手 a/b/x/y、左手 dpad_up/left/down/right；SteamVR 2.17.10 已接受這組綁定）。
//
// 鍵盤（M3a）：自繪虛擬鍵盤見 xrkeyboard.h。鍵盤開啟時射線先對鍵盤求交（優先於影像螢幕），命中時
// 不送滑鼠移動；trigger 按下／放開＝按鍵按下／放開；Shift／Ctrl／Alt／Win 黏滯（按一下保持，下一個一般鍵
// 放開後自動放開）。開關：控制器（Touch 左 menu、Index 左 thumbstick click、Frame 左 view）、熱鍵
// Ctrl+Alt+Shift+K、鍵盤上的 ✕。失去 FOCUSED 或關閉鍵盤時放開按住的鍵與修飾鍵。
// 一般模式下不記錄按了哪些鍵（避免 log 變成 keylogger），只記數量；dev --xr-test-keyboard 才逐鍵記錄。

#pragma once

#include <openxr/openxr.h>

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "xrkeyboard.h"

namespace XrRay {

struct Vec3 {
    float x, y, z;
};

// 以單位四元數旋轉／反旋轉向量
Vec3 rotate(const XrQuaternionf& q, const Vec3& v);
Vec3 rotateInv(const XrQuaternionf& q, const Vec3& v);

// quad：pose 為中心，local +Z 朝向使用者，寬 w、高 h（公尺）。命中回 true，u 往右、v 往下，0..1。
bool intersectQuad(const XrPosef& pose, float w, float h, const Vec3& origin, const Vec3& dir,
                   float* u, float* v, float* t);

// cylinder：pose 為軸心（local +Y 為軸），可見弧段以 local -Z 為中心、張角 centralAngle，
// 高度 = radius·centralAngle / aspect（與 XrCompositionLayerCylinderKHR 一致）。射線從圓柱內部出發。
bool intersectCylinder(const XrPosef& pose, float radius, float centralAngle, float aspect,
                       const Vec3& origin, const Vec3& dir, float* u, float* v, float* t);

// UV → 螢幕上的世界座標（quad／cylinder）；test pointer 與指標 quad 擺放用
Vec3 quadPoint(const XrPosef& pose, float w, float h, float u, float v);
Vec3 cylinderPoint(const XrPosef& pose, float radius, float centralAngle, float aspect, float u, float v);

// One-Euro 濾波（Casiez 2012）。單一維度；x 與 dt 單位不限，截止頻率單位為 Hz。
class OneEuro
{
public:
    OneEuro(float minCutoff, float beta, float dCutoff) : m_MinCutoff(minCutoff), m_Beta(beta), m_DCutoff(dCutoff) {}
    float filter(float x, float dtSec);
    void reset() { m_Init = false; }
    float lastCutoffHz() const { return m_LastCutoff; }

private:
    float m_MinCutoff, m_Beta, m_DCutoff;
    bool m_Init = false;
    float m_X = 0.0f, m_Dx = 0.0f;
    float m_LastCutoff = 0.0f;
};

// 已知案例自測；回傳失敗數。每個案例呼叫 report(名稱, 是否通過, 細節)
int selfTest(const std::function<void(const char*, bool, const QString&)>& report);

}  // namespace XrRay

struct XrInputSink {
    std::function<void(float u, float v)> pointer;         // 命中點（濾波後），0..1
    std::function<void(int button, bool pressed)> button;  // Limelight BUTTON_LEFT／BUTTON_RIGHT
    std::function<void(int amount)> scroll;                 // 高解析捲動量（120＝一格）
    std::function<void(int vk, bool down, uint8_t modifiers)> key;  // 虛擬鍵盤：Win32 VK＋Limelight MODIFIER_*
};

class XrInput
{
public:
    struct Screen {
        bool valid = false;
        bool cylinder = false;
        XrPosef pose = {};  // quad：中心；cylinder：軸心
        float widthM = 0.0f;  // quad 寬
        float heightM = 0.0f;  // quad 高
        float radiusM = 0.0f;  // cylinder
        float angleRad = 0.0f;
        float aspect = 16.0f / 9.0f;
        // 虛擬鍵盤 quad（XrContext 擺放；鍵盤開啟且擺好時 kbValid）
        bool kbValid = false;
        XrPosef kbPose = {};
        float kbWidthM = 0.0f;
        float kbHeightM = 0.0f;
    };

    XrInput(const XrInputSink& sink, bool testPointer, const QString& testKeyboardText = QString());
    ~XrInput();

    // bring-up 執行緒：建 action set、actions、各 profile 綁定、attach、aim action space
    bool create(XrInstance instance, XrSession session, const QStringList& enabledExts, QString* error);
    void destroy();

    XrActionSet actionSet() const { return m_Set; }

    // frame thread：XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED
    void onProfileChanged();

    // frame thread：xrSyncActions 之後每幀呼叫
    void update(XrTime displayTime, XrSpace local, XrSpace view, const Screen& screen, bool focused, uint64_t nowNs);

    // frame thread：目前命中點（世界座標，LOCAL 空間），沒命中回 false
    bool pointerPose(XrPosef* pose) const;

    // frame thread：10 s 彙總（呼叫後清零）
    QString takeStatsLine();

    // ── 虛擬鍵盤 ──
    // 任何執行緒：要求切換開關（熱鍵），frame thread 下一幀處理
    void requestKeyboardToggle() { m_KbToggleReq.fetch_add(1, std::memory_order_acq_rel); }
    // frame thread
    bool keyboardOpen() const { return m_KbOpen; }
    const XrKeyboard& keyboard() const { return m_Kb; }
    uint8_t stickyModifiers() const { return m_Sticky; }
    // 貼圖需要重畫（黏滯修飾鍵變了）；取回即清除
    bool takeKeyboardDirty() { const bool d = m_KbDirty; m_KbDirty = false; return d; }
    // 高亮：目前射線指著的鍵（-1＝沒有）與是否按住中
    int keyboardHover(bool* pressed) const { *pressed = m_KbActiveKey >= 0 && m_KbActiveKey == m_KbHover; return m_KbHover; }

private:
    void releaseAll(const char* why);
    void setKeyboardOpen(bool open, const char* why);
    void releaseKeyboard(const char* why);
    void kbPress(int idx);
    void kbRelease();
    void sendKey(int vk, bool down, uint8_t mods);
    bool syntheticKeyboardRay(XrTime t, XrSpace local, XrSpace view, const Screen& s, uint64_t nowNs,
                              XrRay::Vec3* origin, XrRay::Vec3* dir, bool* trigger);
    void setButton(int idx, bool pressed);
    bool locateAim(int hand, XrTime t, XrSpace local, XrRay::Vec3* origin, XrRay::Vec3* dir);
    bool syntheticRay(XrTime t, XrSpace local, XrSpace view, const Screen& s, uint64_t nowNs,
                      XrRay::Vec3* origin, XrRay::Vec3* dir, bool* trigger);

    XrInputSink m_Sink;
    bool m_TestPointer;
    XrInstance m_Instance = XR_NULL_HANDLE;
    XrSession m_Session = XR_NULL_HANDLE;
    XrPath m_Hands[2] = {XR_NULL_PATH, XR_NULL_PATH};
    XrActionSet m_Set = XR_NULL_HANDLE;
    XrAction m_Aim = XR_NULL_HANDLE;
    XrAction m_Trigger = XR_NULL_HANDLE;
    XrAction m_Squeeze = XR_NULL_HANDLE;
    XrAction m_Secondary = XR_NULL_HANDLE;
    XrAction m_Stick = XR_NULL_HANDLE;
    XrAction m_KbToggleAction = XR_NULL_HANDLE;
    bool m_KbToggleWas[2] = {false, false};
    XrSpace m_AimSpace[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    int m_Dominant = 1;  // 0 左、1 右
    bool m_TriggerDown[2] = {false, false};
    bool m_Pressed[2] = {false, false};  // [0] 左鍵、[1] 右鍵（已送出 press、尚未 release）
    bool m_WasFocused = false;

    XrRay::OneEuro m_FiltU, m_FiltV;
    uint64_t m_LastFilterNs = 0;
    bool m_Hit = false;
    float m_U = 0.0f, m_V = 0.0f;       // 濾波後
    float m_SentU = -1.0f, m_SentV = -1.0f;
    XrPosef m_HitPose = {};
    float m_ScrollAccum = 0.0f;

    // dev test pointer
    uint64_t m_TestStartNs = 0;
    bool m_TestTriggerDown = false;
    int m_TestLastCycle = -1;

    // 虛擬鍵盤
    XrKeyboard m_Kb;
    bool m_KbOpen = false;
    bool m_KbDirty = true;
    std::atomic<int> m_KbToggleReq{0};
    uint8_t m_Sticky = 0;          // 黏滯修飾鍵（XrKeyboard::kMod*）
    int m_KbHover = -1;            // 射線指著的鍵
    int m_KbActiveKey = -1;        // 已送出按下、尚未放開的一般鍵
    uint8_t m_KbActiveMods = 0;    // 該鍵按下時送出的修飾鍵
    bool m_KbTrigPrev = false;
    bool m_TrigOnKb = false;       // 這次 trigger 按下發生在鍵盤上：放開前不轉成滑鼠左鍵
    bool m_KbHit = false;
    uint64_t m_StatKeys = 0;

    // dev --xr-test-keyboard
    struct TestKbStep {
        int key;
    };
    QString m_TestKbText;
    std::vector<TestKbStep> m_TestKbSteps;
    uint64_t m_TestKbStartNs = 0;
    uint64_t m_TestKbShownNs = 0;  // 鍵盤第一次顯示（序列從這裡起算，不受貼圖準備時間影響）
    bool m_TestKbOpened = false;
    bool m_TestKbDone = false;
    int m_TestKbSent = 0;

    // 10 s 統計
    uint64_t m_StatFrames = 0, m_StatHits = 0, m_StatMoves = 0, m_StatButtons = 0, m_StatScrolls = 0;
    double m_StatLatencySumMs = 0.0;
    uint64_t m_StatLatencyN = 0;
};

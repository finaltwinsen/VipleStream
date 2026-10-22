#pragma once

#include <QTimer>
#include <QEvent>

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    // §VR-PLAYER（影片模式的選片頁）：捲動模式——左右搖桿的上下都變成捲動（scrollRequested），LB／RB 捲大半頁；
    // 左搖桿的左右照舊送方向鍵。使用者在頭盔上用雷射指標點、用搖桿捲，清單的焦點不一定在清單上，用方向鍵移焦點捲不動
    Q_INVOKABLE void setScrollMode(bool scrollMode);

    Q_INVOKABLE int getConnectedGamepads();

    // §VR-SHELL-SCROLL：頭盔 VR 介面剛用 XR 搖桿捲了頁面（GUI 執行緒）。之後 300 ms 內這裡不發 scrollRequested
    static void noteExternalScroll();

signals:
    // §UI-SCROLL：手把要求捲動目前這一頁。pages＝幾個可見高度，正值往下（看下面的內容）。
    // 只在 Tab 導覽模式（設定頁、沒有下拉選單開著）或捲動模式（影片選片頁）發出，由那一頁自己捲。
    void scrollRequested(double pages);

protected:
    // §VR-PLAYER：捲動模式開著時，記下視窗收到的滾輪／按鍵／滑鼠鍵（頭盔上用雷射指標時，搖桿到底變成什麼還沒查證）
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    void updateTimerState();
    int pollIntervalMs() const;

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_ScrollMode;
    // §VR-PLAYER：捲動模式下的輸入診斷（每 10 秒一行計數；每一種的第一個事件另外印細節）
    Uint32 m_DiagWindowStart = 0;
    int m_DiagWheel = 0, m_DiagKey = 0, m_DiagButton = 0;
    bool m_DiagWheelLogged = false, m_DiagKeyLogged = false;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    Uint32 m_LastScrollEventTime;   // §UI-SCROLL：右搖桿上一次要求捲動的時刻
    // §UI-SCROLL：下面三個旗標讓 log 在每次 enable() 之後各只印一行（開始輪詢、第一個輸入、第一次捲動）
    bool m_PollLogged;
    bool m_InputLogged;
    bool m_ScrollLogged;
};

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

    Q_INVOKABLE int getConnectedGamepads();

signals:
    // §UI-SCROLL：手把要求捲動目前這一頁。pages＝幾個可見高度，正值往下（看下面的內容）。
    // 只在 Tab 導覽模式（設定頁、沒有下拉選單開著）發出，由那一頁自己捲（SettingsView.qml 的 scrollByPages）。
    void scrollRequested(double pages);

private:
    void sendKey(QEvent::Type type, Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    void updateTimerState();

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    Uint32 m_LastScrollEventTime;   // §UI-SCROLL：右搖桿上一次要求捲動的時刻
    // §UI-SCROLL：下面三個旗標讓 log 在每次 enable() 之後各只印一行（開始輪詢、第一個輸入、第一次捲動）
    bool m_PollLogged;
    bool m_InputLogged;
    bool m_ScrollLogged;
};
